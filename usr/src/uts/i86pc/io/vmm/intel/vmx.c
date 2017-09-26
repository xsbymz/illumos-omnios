/*-
 * Copyright (c) 2011 NetApp, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY NETAPP, INC ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL NETAPP, INC OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: head/sys/amd64/vmm/intel/vmx.c 284174 2015-06-09 00:14:47Z tychon $
 */
/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * Copyright 2015 Pluribus Networks Inc.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD: head/sys/amd64/vmm/intel/vmx.c 284174 2015-06-09 00:14:47Z tychon $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/smp.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <machine/psl.h>
#include <machine/cpufunc.h>
#include <machine/md_var.h>
#include <machine/segments.h>
#include <machine/smp.h>
#include <machine/specialreg.h>
#include <machine/vmparam.h>

#include <machine/vmm.h>
#include <machine/vmm_dev.h>
#include <machine/vmm_instruction_emul.h>
#include "vmm_lapic.h"
#include "vmm_host.h"
#include "vmm_ioport.h"
#include "vmm_ktr.h"
#include "vmm_stat.h"
#include "vatpic.h"
#include "vlapic.h"
#include "vlapic_priv.h"

#include "ept.h"
#include "vmx_cpufunc.h"
#include "vmx.h"
#include "vmx_msr.h"
#include "x86.h"
#include "vmx_controls.h"

#define	PINBASED_CTLS_ONE_SETTING					\
	(PINBASED_EXTINT_EXITING	|				\
	 PINBASED_NMI_EXITING		|				\
	 PINBASED_VIRTUAL_NMI)
#define	PINBASED_CTLS_ZERO_SETTING	0

#define PROCBASED_CTLS_WINDOW_SETTING					\
	(PROCBASED_INT_WINDOW_EXITING	|				\
	 PROCBASED_NMI_WINDOW_EXITING)

#define	PROCBASED_CTLS_ONE_SETTING 					\
	(PROCBASED_SECONDARY_CONTROLS	|				\
	 PROCBASED_IO_EXITING		|				\
	 PROCBASED_MSR_BITMAPS		|				\
	 PROCBASED_CTLS_WINDOW_SETTING	|				\
	 PROCBASED_CR8_LOAD_EXITING	|				\
	 PROCBASED_CR8_STORE_EXITING)
#define	PROCBASED_CTLS_ZERO_SETTING	\
	(PROCBASED_CR3_LOAD_EXITING |	\
	PROCBASED_CR3_STORE_EXITING |	\
	PROCBASED_IO_BITMAPS)

#define	PROCBASED_CTLS2_ONE_SETTING	PROCBASED2_ENABLE_EPT
#define	PROCBASED_CTLS2_ZERO_SETTING	0

#define	VM_EXIT_CTLS_ONE_SETTING					\
	(VM_EXIT_HOST_LMA			|			\
	VM_EXIT_SAVE_EFER			|			\
	VM_EXIT_LOAD_EFER			|			\
	VM_EXIT_LOAD_PAT			|			\
	VM_EXIT_SAVE_PAT			|			\
	VM_EXIT_LOAD_PAT)

#define	VM_EXIT_CTLS_ZERO_SETTING	VM_EXIT_SAVE_DEBUG_CONTROLS

#define	VM_ENTRY_CTLS_ONE_SETTING	(VM_ENTRY_LOAD_EFER | VM_ENTRY_LOAD_PAT)

#define	VM_ENTRY_CTLS_ZERO_SETTING					\
	(VM_ENTRY_LOAD_DEBUG_CONTROLS		|			\
	VM_ENTRY_INTO_SMM			|			\
	VM_ENTRY_DEACTIVATE_DUAL_MONITOR)

#define	HANDLED		1
#define	UNHANDLED	0

static MALLOC_DEFINE(M_VMX, "vmx", "vmx");
static MALLOC_DEFINE(M_VLAPIC, "vlapic", "vlapic");

SYSCTL_DECL(_hw_vmm);
SYSCTL_NODE(_hw_vmm, OID_AUTO, vmx, CTLFLAG_RW, NULL, NULL);

int vmxon_enabled[MAXCPU];
static char vmxon_region[MAXCPU][PAGE_SIZE] __aligned(PAGE_SIZE);
#ifndef	__FreeBSD__
static vm_paddr_t vmxon_region_pa[MAXCPU];
#endif

static uint32_t pinbased_ctls, procbased_ctls, procbased_ctls2;
static uint32_t exit_ctls, entry_ctls;

static uint64_t cr0_ones_mask, cr0_zeros_mask;
SYSCTL_ULONG(_hw_vmm_vmx, OID_AUTO, cr0_ones_mask, CTLFLAG_RD,
	     &cr0_ones_mask, 0, NULL);
SYSCTL_ULONG(_hw_vmm_vmx, OID_AUTO, cr0_zeros_mask, CTLFLAG_RD,
	     &cr0_zeros_mask, 0, NULL);

static uint64_t cr4_ones_mask, cr4_zeros_mask;
SYSCTL_ULONG(_hw_vmm_vmx, OID_AUTO, cr4_ones_mask, CTLFLAG_RD,
	     &cr4_ones_mask, 0, NULL);
SYSCTL_ULONG(_hw_vmm_vmx, OID_AUTO, cr4_zeros_mask, CTLFLAG_RD,
	     &cr4_zeros_mask, 0, NULL);

static int vmx_initialized;
SYSCTL_INT(_hw_vmm_vmx, OID_AUTO, initialized, CTLFLAG_RD,
	   &vmx_initialized, 0, "Intel VMX initialized");

/*
 * Optional capabilities
 */
static int cap_halt_exit;
static int cap_pause_exit;
static int cap_unrestricted_guest;
static int cap_monitor_trap;
static int cap_invpcid;

static int virtual_interrupt_delivery;
SYSCTL_INT(_hw_vmm_vmx, OID_AUTO, virtual_interrupt_delivery, CTLFLAG_RD,
    &virtual_interrupt_delivery, 0, "APICv virtual interrupt delivery support");

static int posted_interrupts;
SYSCTL_INT(_hw_vmm_vmx, OID_AUTO, posted_interrupts, CTLFLAG_RD,
    &posted_interrupts, 0, "APICv posted interrupt support");

static int pirvec;
SYSCTL_INT(_hw_vmm_vmx, OID_AUTO, posted_interrupt_vector, CTLFLAG_RD,
    &pirvec, 0, "APICv posted interrupt vector");

static struct unrhdr *vpid_unr;
static u_int vpid_alloc_failed;
SYSCTL_UINT(_hw_vmm_vmx, OID_AUTO, vpid_alloc_failed, CTLFLAG_RD,
	    &vpid_alloc_failed, 0, NULL);

/*
 * Use the last page below 4GB as the APIC access address. This address is
 * occupied by the boot firmware so it is guaranteed that it will not conflict
 * with a page in system memory.
 */
#define	APIC_ACCESS_ADDRESS	0xFFFFF000

static int vmx_getdesc(void *arg, int vcpu, int reg, struct seg_desc *desc);
static int vmx_getreg(void *arg, int vcpu, int reg, uint64_t *retval);
static int vmxctx_setreg(struct vmxctx *vmxctx, int reg, uint64_t val);
static void vmx_inject_pir(struct vlapic *vlapic);

#ifdef KTR
static const char *
exit_reason_to_str(int reason)
{
	static char reasonbuf[32];

	switch (reason) {
	case EXIT_REASON_EXCEPTION:
		return "exception";
	case EXIT_REASON_EXT_INTR:
		return "extint";
	case EXIT_REASON_TRIPLE_FAULT:
		return "triplefault";
	case EXIT_REASON_INIT:
		return "init";
	case EXIT_REASON_SIPI:
		return "sipi";
	case EXIT_REASON_IO_SMI:
		return "iosmi";
	case EXIT_REASON_SMI:
		return "smi";
	case EXIT_REASON_INTR_WINDOW:
		return "intrwindow";
	case EXIT_REASON_NMI_WINDOW:
		return "nmiwindow";
	case EXIT_REASON_TASK_SWITCH:
		return "taskswitch";
	case EXIT_REASON_CPUID:
		return "cpuid";
	case EXIT_REASON_GETSEC:
		return "getsec";
	case EXIT_REASON_HLT:
		return "hlt";
	case EXIT_REASON_INVD:
		return "invd";
	case EXIT_REASON_INVLPG:
		return "invlpg";
	case EXIT_REASON_RDPMC:
		return "rdpmc";
	case EXIT_REASON_RDTSC:
		return "rdtsc";
	case EXIT_REASON_RSM:
		return "rsm";
	case EXIT_REASON_VMCALL:
		return "vmcall";
	case EXIT_REASON_VMCLEAR:
		return "vmclear";
	case EXIT_REASON_VMLAUNCH:
		return "vmlaunch";
	case EXIT_REASON_VMPTRLD:
		return "vmptrld";
	case EXIT_REASON_VMPTRST:
		return "vmptrst";
	case EXIT_REASON_VMREAD:
		return "vmread";
	case EXIT_REASON_VMRESUME:
		return "vmresume";
	case EXIT_REASON_VMWRITE:
		return "vmwrite";
	case EXIT_REASON_VMXOFF:
		return "vmxoff";
	case EXIT_REASON_VMXON:
		return "vmxon";
	case EXIT_REASON_CR_ACCESS:
		return "craccess";
	case EXIT_REASON_DR_ACCESS:
		return "draccess";
	case EXIT_REASON_INOUT:
		return "inout";
	case EXIT_REASON_RDMSR:
		return "rdmsr";
	case EXIT_REASON_WRMSR:
		return "wrmsr";
	case EXIT_REASON_INVAL_VMCS:
		return "invalvmcs";
	case EXIT_REASON_INVAL_MSR:
		return "invalmsr";
	case EXIT_REASON_MWAIT:
		return "mwait";
	case EXIT_REASON_MTF:
		return "mtf";
	case EXIT_REASON_MONITOR:
		return "monitor";
	case EXIT_REASON_PAUSE:
		return "pause";
	case EXIT_REASON_MCE:
		return "mce";
	case EXIT_REASON_TPR:
		return "tpr";
	case EXIT_REASON_APIC_ACCESS:
		return "apic-access";
	case EXIT_REASON_GDTR_IDTR:
		return "gdtridtr";
	case EXIT_REASON_LDTR_TR:
		return "ldtrtr";
	case EXIT_REASON_EPT_FAULT:
		return "eptfault";
	case EXIT_REASON_EPT_MISCONFIG:
		return "eptmisconfig";
	case EXIT_REASON_INVEPT:
		return "invept";
	case EXIT_REASON_RDTSCP:
		return "rdtscp";
	case EXIT_REASON_VMX_PREEMPT:
		return "vmxpreempt";
	case EXIT_REASON_INVVPID:
		return "invvpid";
	case EXIT_REASON_WBINVD:
		return "wbinvd";
	case EXIT_REASON_XSETBV:
		return "xsetbv";
	case EXIT_REASON_APIC_WRITE:
		return "apic-write";
	default:
		snprintf(reasonbuf, sizeof(reasonbuf), "%d", reason);
		return (reasonbuf);
	}
}

#ifdef SETJMP_TRACE
static const char *
vmx_setjmp_rc2str(int rc)
{
	switch (rc) {
	case VMX_RETURN_DIRECT:
		return "direct";
	case VMX_RETURN_LONGJMP:
		return "longjmp";
	case VMX_RETURN_VMRESUME:
		return "vmresume";
	case VMX_RETURN_VMLAUNCH:
		return "vmlaunch";
	case VMX_RETURN_AST:
		return "ast";
	default:
		return "unknown";
	}
}

#define	SETJMP_TRACE(vmx, vcpu, vmxctx, regname)			  \
	VMM_CTR1((vmx)->vm, (vcpu), "setjmp trace " #regname " 0x%016lx", \
		 (vmxctx)->regname)

static void
vmx_setjmp_trace(struct vmx *vmx, int vcpu, struct vmxctx *vmxctx, int rc)
{
	uint64_t host_rip, host_rsp;

	if (vmxctx != &vmx->ctx[vcpu])
		panic("vmx_setjmp_trace: invalid vmxctx %p; should be %p",
			vmxctx, &vmx->ctx[vcpu]);

	VMM_CTR1((vmx)->vm, (vcpu), "vmxctx = %p", vmxctx);
	VMM_CTR2((vmx)->vm, (vcpu), "setjmp return code %s(%d)",
		 vmx_setjmp_rc2str(rc), rc);

	host_rsp = host_rip = ~0;
	vmread(VMCS_HOST_RIP, &host_rip);
	vmread(VMCS_HOST_RSP, &host_rsp);
	VMM_CTR2((vmx)->vm, (vcpu), "vmcs host_rip 0x%016lx, host_rsp 0x%016lx",
		 host_rip, host_rsp);

	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r15);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r14);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r13);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_r12);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rbp);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rsp);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rbx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, host_rip);

	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rdi);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rsi);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rdx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rcx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r8);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r9);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rax);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rbx);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_rbp);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r10);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r11);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r12);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r13);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r14);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_r15);
	SETJMP_TRACE(vmx, vcpu, vmxctx, guest_cr2);
}
#endif
#else
static void __inline
vmx_setjmp_trace(struct vmx *vmx, int vcpu, struct vmxctx *vmxctx, int rc)
{
	return;
}
#endif	/* KTR */

static int
vmx_allow_x2apic_msrs(struct vmx *vmx)
{
	int i, error;

	error = 0;

	/*
	 * Allow readonly access to the following x2APIC MSRs from the guest.
	 */
	error += guest_msr_ro(vmx, MSR_APIC_ID);
	error += guest_msr_ro(vmx, MSR_APIC_VERSION);
	error += guest_msr_ro(vmx, MSR_APIC_LDR);
	error += guest_msr_ro(vmx, MSR_APIC_SVR);

	for (i = 0; i < 8; i++)
		error += guest_msr_ro(vmx, MSR_APIC_ISR0 + i);

	for (i = 0; i < 8; i++)
		error += guest_msr_ro(vmx, MSR_APIC_TMR0 + i);
	
	for (i = 0; i < 8; i++)
		error += guest_msr_ro(vmx, MSR_APIC_IRR0 + i);

	error += guest_msr_ro(vmx, MSR_APIC_ESR);
	error += guest_msr_ro(vmx, MSR_APIC_LVT_TIMER);
	error += guest_msr_ro(vmx, MSR_APIC_LVT_THERMAL);
	error += guest_msr_ro(vmx, MSR_APIC_LVT_PCINT);
	error += guest_msr_ro(vmx, MSR_APIC_LVT_LINT0);
	error += guest_msr_ro(vmx, MSR_APIC_LVT_LINT1);
	error += guest_msr_ro(vmx, MSR_APIC_LVT_ERROR);
	error += guest_msr_ro(vmx, MSR_APIC_ICR_TIMER);
	error += guest_msr_ro(vmx, MSR_APIC_DCR_TIMER);
	error += guest_msr_ro(vmx, MSR_APIC_ICR);

	/*
	 * Allow TPR, EOI and SELF_IPI MSRs to be read and written by the guest.
	 *
	 * These registers get special treatment described in the section
	 * "Virtualizing MSR-Based APIC Accesses".
	 */
	error += guest_msr_rw(vmx, MSR_APIC_TPR);
	error += guest_msr_rw(vmx, MSR_APIC_EOI);
	error += guest_msr_rw(vmx, MSR_APIC_SELF_IPI);

	return (error);
}

u_long
vmx_fix_cr0(u_long cr0)
{

	return ((cr0 | cr0_ones_mask) & ~cr0_zeros_mask);
}

u_long
vmx_fix_cr4(u_long cr4)
{

	return ((cr4 | cr4_ones_mask) & ~cr4_zeros_mask);
}

static void
vpid_free(int vpid)
{
	if (vpid < 0 || vpid > 0xffff)
		panic("vpid_free: invalid vpid %d", vpid);

	/*
	 * VPIDs [0,VM_MAXCPU] are special and are not allocated from
	 * the unit number allocator.
	 */

	if (vpid > VM_MAXCPU)
		free_unr(vpid_unr, vpid);
}

static void
vpid_alloc(uint16_t *vpid, int num)
{
	int i, x;

	if (num <= 0 || num > VM_MAXCPU)
		panic("invalid number of vpids requested: %d", num);

	/*
	 * If the "enable vpid" execution control is not enabled then the
	 * VPID is required to be 0 for all vcpus.
	 */
	if ((procbased_ctls2 & PROCBASED2_ENABLE_VPID) == 0) {
		for (i = 0; i < num; i++)
			vpid[i] = 0;
		return;
	}

	/*
	 * Allocate a unique VPID for each vcpu from the unit number allocator.
	 */
	for (i = 0; i < num; i++) {
		x = alloc_unr(vpid_unr);
		if (x == -1)
			break;
		else
			vpid[i] = x;
	}

	if (i < num) {
		atomic_add_int(&vpid_alloc_failed, 1);

		/*
		 * If the unit number allocator does not have enough unique
		 * VPIDs then we need to allocate from the [1,VM_MAXCPU] range.
		 *
		 * These VPIDs are not be unique across VMs but this does not
		 * affect correctness because the combined mappings are also
		 * tagged with the EP4TA which is unique for each VM.
		 *
		 * It is still sub-optimal because the invvpid will invalidate
		 * combined mappings for a particular VPID across all EP4TAs.
		 */
		while (i-- > 0)
			vpid_free(vpid[i]);

		for (i = 0; i < num; i++)
			vpid[i] = i + 1;
	}
}

static void
vpid_init(void)
{
	/*
	 * VPID 0 is required when the "enable VPID" execution control is
	 * disabled.
	 *
	 * VPIDs [1,VM_MAXCPU] are used as the "overflow namespace" when the
	 * unit number allocator does not have sufficient unique VPIDs to
	 * satisfy the allocation.
	 *
	 * The remaining VPIDs are managed by the unit number allocator.
	 */
	vpid_unr = new_unrhdr(VM_MAXCPU + 1, 0xffff, NULL);
}

#ifndef	__FreeBSD__
static void
msr_save_area_init(struct msr_entry *g_area, int *g_count)
{
	int cnt;

	static struct msr_entry guest_msrs[] = {
		{ MSR_KGSBASE, 0, 0 },
		{ MSR_LSTAR, 0, 0 },
		{ MSR_CSTAR, 0, 0 },
		{ MSR_STAR, 0, 0 },
		{ MSR_SF_MASK, 0, 0 },
	};

	cnt = sizeof(guest_msrs) / sizeof(guest_msrs[0]);
	if (cnt > GUEST_MSR_MAX_ENTRIES)
		panic("guest msr save area overrun");
	bcopy(guest_msrs, g_area, sizeof(guest_msrs));
	*g_count = cnt;
}

static void
host_msr_save_area_init(struct msr_entry *h_area, int *h_count)
{
	int i, cnt;

	static struct msr_entry host_msrs[] = {
		{ MSR_LSTAR, 0, 0 },
		{ MSR_CSTAR, 0, 0 },
		{ MSR_STAR, 0, 0 },
		{ MSR_SF_MASK, 0, 0 },
	};

	cnt = sizeof(host_msrs) / sizeof(host_msrs[0]);
	if (cnt > HOST_MSR_MAX_ENTRIES)
		panic("host msr save area overrun");
	for (i = 0; i < cnt; i++) {
		host_msrs[i].val = rdmsr(host_msrs[i].index);
	}
	bcopy(host_msrs, h_area, sizeof(host_msrs));
	*h_count = cnt;
}
#endif

static void
vmx_disable(void *arg __unused)
{
	struct invvpid_desc invvpid_desc = { 0 };
	struct invept_desc invept_desc = { 0 };

	if (vmxon_enabled[curcpu]) {
		/*
		 * See sections 25.3.3.3 and 25.3.3.4 in Intel Vol 3b.
		 *
		 * VMXON or VMXOFF are not required to invalidate any TLB
		 * caching structures. This prevents potential retention of
		 * cached information in the TLB between distinct VMX episodes.
		 */
		invvpid(INVVPID_TYPE_ALL_CONTEXTS, invvpid_desc);
		invept(INVEPT_TYPE_ALL_CONTEXTS, invept_desc);
		vmxoff();
	}
	load_cr4(rcr4() & ~CR4_VMXE);
}

static int
vmx_cleanup(void)
{
	
#ifdef	__FreeBSD__
	if (pirvec != 0)
		vmm_ipi_free(pirvec);
#endif

	if (vpid_unr != NULL) {
		delete_unrhdr(vpid_unr);
		vpid_unr = NULL;
	}

	smp_rendezvous(NULL, vmx_disable, NULL, NULL);

	return (0);
}

static void
vmx_enable(void *arg __unused)
{
	int error;
	uint64_t feature_control;

	feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);
	if ((feature_control & IA32_FEATURE_CONTROL_LOCK) == 0 ||
	    (feature_control & IA32_FEATURE_CONTROL_VMX_EN) == 0) {
		wrmsr(MSR_IA32_FEATURE_CONTROL,
		    feature_control | IA32_FEATURE_CONTROL_VMX_EN |
		    IA32_FEATURE_CONTROL_LOCK);
	}

	load_cr4(rcr4() | CR4_VMXE);

	*(uint32_t *)vmxon_region[curcpu] = vmx_revision();
#ifdef	__FreeBSD__
	error = vmxon(vmxon_region[curcpu]);
#else
	error = vmxon_pa(vmxon_region_pa[curcpu]);
	ASSERT(error == 0);
#endif
	if (error == 0)
		vmxon_enabled[curcpu] = 1;
}

static int
vmx_init(void)
{
#define	X86FSET_VMX	35
	extern uchar_t x86_featureset[];
	extern boolean_t is_x86_feature(void *featureset, uint_t feature);
	int error;
	uint64_t fixed0, fixed1, feature_control;
	uint32_t tmp;
#ifndef	__FreeBSD__
	int i;
#endif

	/* CPUID.1:ECX[bit 5] must be 1 for processor to support VMX */
#ifdef	__FreeBSD__
	if (!(cpu_feature2 & CPUID2_VMX)) {
		printf("vmx_init: processor does not support VMX operation\n");
		return (ENXIO);
	}
#else
	if (!is_x86_feature(x86_featureset, X86FSET_VMX)) {
		cmn_err(CE_WARN, "vmx_init: processor does not support VMX operation\n");
	}
#endif

	/*
	 * Verify that MSR_IA32_FEATURE_CONTROL lock and VMXON enable bits
	 * are set (bits 0 and 2 respectively).
	 */
	feature_control = rdmsr(MSR_IA32_FEATURE_CONTROL);
	if ((feature_control & IA32_FEATURE_CONTROL_LOCK) == 1 &&
	    (feature_control & IA32_FEATURE_CONTROL_VMX_EN) == 0) {
		printf("vmx_init: VMX operation disabled by BIOS\n");
		return (ENXIO);
	}

	/* Check support for primary processor-based VM-execution controls */
	error = vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
			       MSR_VMX_TRUE_PROCBASED_CTLS,
			       PROCBASED_CTLS_ONE_SETTING,
			       PROCBASED_CTLS_ZERO_SETTING, &procbased_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired primary "
		       "processor-based controls\n");
		return (error);
	}

	/* Clear the processor-based ctl bits that are set on demand */
	procbased_ctls &= ~PROCBASED_CTLS_WINDOW_SETTING;

	/* Check support for secondary processor-based VM-execution controls */
	error = vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS2,
			       MSR_VMX_PROCBASED_CTLS2,
			       PROCBASED_CTLS2_ONE_SETTING,
			       PROCBASED_CTLS2_ZERO_SETTING, &procbased_ctls2);
	if (error) {
		printf("vmx_init: processor does not support desired secondary "
		       "processor-based controls\n");
		return (error);
	}

	/* Check support for VPID */
	error = vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS2, MSR_VMX_PROCBASED_CTLS2,
			       PROCBASED2_ENABLE_VPID, 0, &tmp);
	if (error == 0)
		procbased_ctls2 |= PROCBASED2_ENABLE_VPID;

	/* Check support for pin-based VM-execution controls */
	error = vmx_set_ctlreg(MSR_VMX_PINBASED_CTLS,
			       MSR_VMX_TRUE_PINBASED_CTLS,
			       PINBASED_CTLS_ONE_SETTING,
			       PINBASED_CTLS_ZERO_SETTING, &pinbased_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired "
		       "pin-based controls\n");
		return (error);
	}

	/* Check support for VM-exit controls */
	error = vmx_set_ctlreg(MSR_VMX_EXIT_CTLS, MSR_VMX_TRUE_EXIT_CTLS,
			       VM_EXIT_CTLS_ONE_SETTING,
			       VM_EXIT_CTLS_ZERO_SETTING,
			       &exit_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired "
		    "exit controls\n");
		return (error);
	}

	/* Check support for VM-entry controls */
	error = vmx_set_ctlreg(MSR_VMX_ENTRY_CTLS, MSR_VMX_TRUE_ENTRY_CTLS,
	    VM_ENTRY_CTLS_ONE_SETTING, VM_ENTRY_CTLS_ZERO_SETTING,
	    &entry_ctls);
	if (error) {
		printf("vmx_init: processor does not support desired "
		    "entry controls\n");
		return (error);
	}

	/*
	 * Check support for optional features by testing them
	 * as individual bits
	 */
	cap_halt_exit = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
					MSR_VMX_TRUE_PROCBASED_CTLS,
					PROCBASED_HLT_EXITING, 0,
					&tmp) == 0);

	cap_monitor_trap = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
					MSR_VMX_PROCBASED_CTLS,
					PROCBASED_MTF, 0,
					&tmp) == 0);

	cap_pause_exit = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS,
					 MSR_VMX_TRUE_PROCBASED_CTLS,
					 PROCBASED_PAUSE_EXITING, 0,
					 &tmp) == 0);

	cap_unrestricted_guest = (vmx_set_ctlreg(MSR_VMX_PROCBASED_CTLS2,
					MSR_VMX_PROCBASED_CTLS2,
					PROCBASED2_UNRESTRICTED_GUEST, 0,
				        &tmp) == 0);

	/* Initialize EPT */
	error = ept_init();
	if (error) {
		printf("vmx_init: ept initialization failed (%d)\n", error);
		return (error);
	}

	/*
	 * Stash the cr0 and cr4 bits that must be fixed to 0 or 1
	 */
	fixed0 = rdmsr(MSR_VMX_CR0_FIXED0);
	fixed1 = rdmsr(MSR_VMX_CR0_FIXED1);
	cr0_ones_mask = fixed0 & fixed1;
	cr0_zeros_mask = ~fixed0 & ~fixed1;

	/*
	 * CR0_PE and CR0_PG can be set to zero in VMX non-root operation
	 * if unrestricted guest execution is allowed.
	 */
	if (cap_unrestricted_guest)
		cr0_ones_mask &= ~(CR0_PG | CR0_PE);

	/*
	 * Do not allow the guest to set CR0_NW or CR0_CD.
	 */
	cr0_zeros_mask |= (CR0_NW | CR0_CD);

	fixed0 = rdmsr(MSR_VMX_CR4_FIXED0);
	fixed1 = rdmsr(MSR_VMX_CR4_FIXED1);
	cr4_ones_mask = fixed0 & fixed1;
	cr4_zeros_mask = ~fixed0 & ~fixed1;

#ifndef	__FreeBSD__
	for (i = 0; i < MAXCPU; i++) {
		vmxon_region_pa[i] = vtophys(&vmxon_region[i]);
	}
#endif

	vpid_init();

	vmx_msr_init();

	/* enable VMX operation */
	smp_rendezvous(NULL, vmx_enable, NULL, NULL);

	vmx_initialized = 1;

	return (0);
}

static int
vmx_setup_cr_shadow(int which, struct vmcs *vmcs, uint32_t initial)
{
	int error, mask_ident, shadow_ident;
	uint64_t mask_value;

	if (which != 0 && which != 4)
		panic("vmx_setup_cr_shadow: unknown cr%d", which);

	if (which == 0) {
		mask_ident = VMCS_CR0_MASK;
		mask_value = cr0_ones_mask | cr0_zeros_mask;
		shadow_ident = VMCS_CR0_SHADOW;
	} else {
		mask_ident = VMCS_CR4_MASK;
		mask_value = cr4_ones_mask | cr4_zeros_mask;
		shadow_ident = VMCS_CR4_SHADOW;
	}

	error = vmcs_setreg(vmcs, 0, VMCS_IDENT(mask_ident), mask_value);
	if (error)
		return (error);

	error = vmcs_setreg(vmcs, 0, VMCS_IDENT(shadow_ident), initial);
	if (error)
		return (error);

	return (0);
}
#define	vmx_setup_cr0_shadow(vmcs,init)	vmx_setup_cr_shadow(0, (vmcs), (init))
#define	vmx_setup_cr4_shadow(vmcs,init)	vmx_setup_cr_shadow(4, (vmcs), (init))

static void *
vmx_vminit(struct vm *vm)
{
	uint16_t vpid[VM_MAXCPU];
	int i, error, guest_msr_count;
#ifndef	__FreeBSD__
	int host_msr_count;
#endif
	struct vmx *vmx;
	struct vmcs *vmcs;

	vmx = malloc(sizeof(struct vmx), M_VMX, M_WAITOK | M_ZERO);
	if ((uintptr_t)vmx & PAGE_MASK) {
		panic("malloc of struct vmx not aligned on %d byte boundary",
		      PAGE_SIZE);
	}
	vmx->vm = vm;

	/*
	 * Clean up EPTP-tagged guest physical and combined mappings
	 *
	 * VMX transitions are not required to invalidate any guest physical
	 * mappings. So, it may be possible for stale guest physical mappings
	 * to be present in the processor TLBs.
	 *
	 * Combined mappings for this EP4TA are also invalidated for all VPIDs.
	 */
	ept_invalidate_mappings(vtophys(vmx->pml4ept));

	msr_bitmap_initialize(vmx->msr_bitmap);

	/*
	 * It is safe to allow direct access to MSR_GSBASE and MSR_FSBASE.
	 * The guest FSBASE and GSBASE are saved and restored during
	 * vm-exit and vm-entry respectively. The host FSBASE and GSBASE are
	 * always restored from the vmcs host state area on vm-exit.
	 *
	 * The SYSENTER_CS/ESP/EIP MSRs are identical to FS/GSBASE in
	 * how they are saved/restored so can be directly accessed by the
	 * guest.
	 *
	 * MSR_EFER is saved and restored in the guest VMCS area on a
	 * VM exit and entry respectively. It is also restored from the
	 * host VMCS area on a VM exit.
	 *
	 * MSR_PAT is saved and restored in the guest VMCS are on a VM exit
	 * and entry respectively. It is also restored from the host VMCS
	 * area on a VM exit.
	 *
	 * The TSC MSR is exposed read-only. Writes are disallowed as
	 * that will impact the host TSC.  If the guest does a write
	 * the "use TSC offsetting" execution control is enabled and the
	 * difference between the host TSC and the guest TSC is written
	 * into the TSC offset in the VMCS.
	 */
	if (guest_msr_rw(vmx, MSR_GSBASE) ||
	    guest_msr_rw(vmx, MSR_FSBASE) ||
	    guest_msr_rw(vmx, MSR_SYSENTER_CS_MSR) ||
	    guest_msr_rw(vmx, MSR_SYSENTER_ESP_MSR) ||
	    guest_msr_rw(vmx, MSR_SYSENTER_EIP_MSR) ||
	    guest_msr_rw(vmx, MSR_EFER) ||
	    guest_msr_rw(vmx, MSR_PAT) ||
	    guest_msr_ro(vmx, MSR_TSC))
		panic("vmx_vminit: error setting guest msr access");

	vpid_alloc(vpid, VM_MAXCPU);

	for (i = 0; i < VM_MAXCPU; i++) {
		vmcs = &vmx->vmcs[i];
		vmcs->identifier = vmx_revision();
		error = vmclear(vmcs);
		if (error != 0) {
			panic("vmx_vminit: vmclear error %d on vcpu %d\n",
			      error, i);
		}

		vmx_msr_guest_init(vmx, i);

		error = vmcs_set_defaults(vmcs,
					  (u_long)vmx_longjmp,
					  (u_long)&vmx->ctx[i],
					  vtophys(vmx->pml4ept),
					  pinbased_ctls,
					  procbased_ctls,
					  procbased_ctls2,
					  exit_ctls, entry_ctls,
					  vtophys(vmx->msr_bitmap),
					  vpid[i]);

		if (error != 0)
			panic("vmx_vminit: vmcs_set_defaults error %d", error);

		vmx->cap[i].set = 0;
		vmx->cap[i].proc_ctls = procbased_ctls;

		vmx->state[i].lastcpu = -1;
		vmx->state[i].vpid = vpid[i];

#ifndef	__FreeBSD__
		msr_save_area_init(vmx->guest_msrs[i], &guest_msr_count);

		error = vmcs_set_msr_save(vmcs, vtophys(vmx->guest_msrs[i]),
		    guest_msr_count);
		if (error != 0)
			panic("vmcs_set_msr_save error %d", error);

		host_msr_save_area_init(vmx->host_msrs[i], &host_msr_count);

		error = vmcs_set_host_msr_save(&vmx->vmcs[i],
					       vtophys(vmx->host_msrs[i]),
					       host_msr_count);
		if (error != 0)
			panic("vmcs_set_msr_save error %d", error);
#endif

		/*
		 * Set up the CR0/4 shadows, and init the read shadow
		 * to the power-on register value from the Intel Sys Arch.
		 *  CR0 - 0x60000010
		 *  CR4 - 0
		 */
		error = vmx_setup_cr0_shadow(vmcs, 0x60000010);
		if (error != 0)
			panic("vmx_setup_cr0_shadow %d", error);

		error = vmx_setup_cr4_shadow(vmcs, 0);
		if (error != 0)
			panic("vmx_setup_cr4_shadow %d", error);
	}

	return (vmx);
}

static int
vmx_handle_cpuid(struct vm *vm, int vcpu, struct vmxctx *vmxctx)
{
	int handled, func;
	
	func = vmxctx->guest_rax;

	handled = x86_emulate_cpuid(vm, vcpu,
				    (uint32_t*)(&vmxctx->guest_rax),
				    (uint32_t*)(&vmxctx->guest_rbx),
				    (uint32_t*)(&vmxctx->guest_rcx),
				    (uint32_t*)(&vmxctx->guest_rdx));
	return (handled);
}

static __inline void
vmx_run_trace(struct vmx *vmx, int vcpu)
{
#ifdef KTR
	VCPU_CTR1(vmx->vm, vcpu, "Resume execution at %#lx", vmcs_guest_rip());
#endif
}

static __inline void
vmx_exit_trace(struct vmx *vmx, int vcpu, uint64_t rip, uint32_t exit_reason,
	       int handled)
{
#ifdef KTR
	VCPU_CTR3(vmx->vm, vcpu, "%s %s vmexit at 0x%0lx",
		 handled ? "handled" : "unhandled",
		 exit_reason_to_str(exit_reason), rip);
#endif
}

static __inline void
vmx_astpending_trace(struct vmx *vmx, int vcpu, uint64_t rip)
{
#ifdef KTR
	VCPU_CTR1(vmx->vm, vcpu, "astpending vmexit at 0x%0lx", rip);
#endif
}

static void
vmx_set_pcpu_defaults(struct vmx *vmx, int vcpu)
{
	struct vmxstate *vmxstate;
	struct invvpid_desc invvpid_desc = { 0 };
#ifndef	__FreeBSD__
	desctbr_t idtr, gdtr;
#endif

	vmxstate = &vmx->state[vcpu];
	vmcs_write(VMCS_HOST_FS_BASE, vmm_get_host_fsbase());
	if (vmxstate->lastcpu == curcpu)
		return;

	vmxstate->lastcpu = curcpu;

	vmm_stat_incr(vmx->vm, vcpu, VCPU_MIGRATIONS, 1);

	vmcs_write(VMCS_HOST_TR_BASE, vmm_get_host_trbase());
	vmcs_write(VMCS_HOST_GDTR_BASE, vmm_get_host_gdtrbase());
	vmcs_write(VMCS_HOST_GS_BASE, vmm_get_host_gsbase());

#ifndef	__FreeBSD__
	vmcs_write(VMCS_HOST_IA32_SYSENTER_CS, rdmsr(MSR_SYSENTER_CS_MSR));
	vmcs_write(VMCS_HOST_IA32_SYSENTER_ESP, rdmsr(MSR_SYSENTER_ESP_MSR));
	vmcs_write(VMCS_HOST_IA32_SYSENTER_EIP, rdmsr(MSR_SYSENTER_EIP_MSR));
#endif

	/*
	 * If we are using VPIDs then invalidate all mappings tagged with 'vpid'
	 *
	 * We do this because this vcpu was executing on a different host
	 * cpu when it last ran. We do not track whether it invalidated
	 * mappings associated with its 'vpid' during that run. So we must
	 * assume that the mappings associated with 'vpid' on 'curcpu' are
	 * stale and invalidate them.
	 *
	 * Note that we incur this penalty only when the scheduler chooses to
	 * move the thread associated with this vcpu between host cpus.
	 *
	 * Note also that this will invalidate mappings tagged with 'vpid'
	 * for "all" EP4TAs.
	 */
	if (vmxstate->vpid != 0) {
		invvpid_desc.vpid = vmxstate->vpid;
		invvpid(INVVPID_TYPE_SINGLE_CONTEXT, invvpid_desc);
	}
}

static void 
vm_exit_update_rip(struct vm_exit *vmexit)
{
	int error;

	error = vmwrite(VMCS_GUEST_RIP, vmexit->rip + vmexit->inst_length);
	if (error)
		panic("vmx_run: error %d writing to VMCS_GUEST_RIP", error);
}

/*
 * We depend on 'procbased_ctls' to have the Interrupt Window Exiting bit set.
 */
CTASSERT((PROCBASED_CTLS_ONE_SETTING & PROCBASED_INT_WINDOW_EXITING) != 0);

static void __inline
vmx_set_int_window_exiting(struct vmx *vmx, int vcpu)
{

	if ((vmx->cap[vcpu].proc_ctls & PROCBASED_INT_WINDOW_EXITING) == 0) {
		vmx->cap[vcpu].proc_ctls |= PROCBASED_INT_WINDOW_EXITING;
		vmcs_write(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
		VCPU_CTR0(vmx->vm, vcpu, "Enabling interrupt window exiting");
	}
}

static void __inline
vmx_clear_int_window_exiting(struct vmx *vmx, int vcpu)
{

#ifdef	__FreeBSD__
	KASSERT((vmx->cap[vcpu].proc_ctls & PROCBASED_INT_WINDOW_EXITING) != 0,
	    ("intr_window_exiting not set: %#x", vmx->cap[vcpu].proc_ctls));
#else
	KASSERT((vmx->cap[vcpu].proc_ctls & PROCBASED_INT_WINDOW_EXITING) != 0,
	    ("intr_window_exiting not set: %x", vmx->cap[vcpu].proc_ctls));
#endif
	vmx->cap[vcpu].proc_ctls &= ~PROCBASED_INT_WINDOW_EXITING;
	vmcs_write(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
	VCPU_CTR0(vmx->vm, vcpu, "Disabling interrupt window exiting");
}

static void __inline
vmx_set_nmi_window_exiting(struct vmx *vmx, int vcpu)
{

	if ((vmx->cap[vcpu].proc_ctls & PROCBASED_NMI_WINDOW_EXITING) == 0) {
		vmx->cap[vcpu].proc_ctls |= PROCBASED_NMI_WINDOW_EXITING;
		vmcs_write(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
		VCPU_CTR0(vmx->vm, vcpu, "Enabling NMI window exiting");
	}
}

static void __inline
vmx_clear_nmi_window_exiting(struct vmx *vmx, int vcpu)
{

#ifdef	__FreeBSD__
	KASSERT((vmx->cap[vcpu].proc_ctls & PROCBASED_NMI_WINDOW_EXITING) != 0,
	    ("nmi_window_exiting not set %#x", vmx->cap[vcpu].proc_ctls));
#else
	KASSERT((vmx->cap[vcpu].proc_ctls & PROCBASED_NMI_WINDOW_EXITING) != 0,
	    ("nmi_window_exiting not set %x", vmx->cap[vcpu].proc_ctls));
#endif
	vmx->cap[vcpu].proc_ctls &= ~PROCBASED_NMI_WINDOW_EXITING;
	vmcs_write(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
	VCPU_CTR0(vmx->vm, vcpu, "Disabling NMI window exiting");
}

int
vmx_set_tsc_offset(struct vmx *vmx, int vcpu, uint64_t offset)
{
	int error;

	if ((vmx->cap[vcpu].proc_ctls & PROCBASED_TSC_OFFSET) == 0) {
		vmx->cap[vcpu].proc_ctls |= PROCBASED_TSC_OFFSET;
		vmcs_write(VMCS_PRI_PROC_BASED_CTLS, vmx->cap[vcpu].proc_ctls);
		VCPU_CTR0(vmx->vm, vcpu, "Enabling TSC offsetting");
	}

	error = vmwrite(VMCS_TSC_OFFSET, offset);

	return (error);
}

#define	NMI_BLOCKING	(VMCS_INTERRUPTIBILITY_NMI_BLOCKING |		\
			 VMCS_INTERRUPTIBILITY_MOVSS_BLOCKING)
#define	HWINTR_BLOCKING	(VMCS_INTERRUPTIBILITY_STI_BLOCKING |		\
			 VMCS_INTERRUPTIBILITY_MOVSS_BLOCKING)

static void
vmx_inject_nmi(struct vmx *vmx, int vcpu)
{
	uint32_t gi, info;

	gi = vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
#ifdef	__FreeBSD__
	KASSERT((gi & NMI_BLOCKING) == 0, ("vmx_inject_nmi: invalid guest "
	    "interruptibility-state %#x", gi));
#else
	KASSERT((gi & NMI_BLOCKING) == 0, ("vmx_inject_nmi: invalid guest "
	    "interruptibility-state %x", gi));
#endif

	info = vmcs_read(VMCS_ENTRY_INTR_INFO);
#ifdef	__FreeBSD__
	KASSERT((info & VMCS_INTR_VALID) == 0, ("vmx_inject_nmi: invalid "
	    "VM-entry interruption information %#x", info));
#else
	KASSERT((info & VMCS_INTR_VALID) == 0, ("vmx_inject_nmi: invalid "
	    "VM-entry interruption information %x", info));
#endif

	/*
	 * Inject the virtual NMI. The vector must be the NMI IDT entry
	 * or the VMCS entry check will fail.
	 */
	info = IDT_NMI | VMCS_INTR_T_NMI | VMCS_INTR_VALID;
	vmcs_write(VMCS_ENTRY_INTR_INFO, info);

	VCPU_CTR0(vmx->vm, vcpu, "Injecting vNMI");

	/* Clear the request */
	vm_nmi_clear(vmx->vm, vcpu);
}

static void
vmx_inject_interrupts(struct vmx *vmx, int vcpu, struct vlapic *vlapic)
{
	int vector, need_nmi_exiting, extint_pending;
	uint64_t rflags, entryinfo;
	uint32_t gi, info;

	if (vm_entry_intinfo(vmx->vm, vcpu, &entryinfo)) {
#ifdef	__FreeBSD__
		KASSERT((entryinfo & VMCS_INTR_VALID) != 0, ("%s: entry "
		    "intinfo is not valid: %#lx", __func__, entryinfo));
#else
		KASSERT((entryinfo & VMCS_INTR_VALID) != 0, ("%s: entry "
		    "intinfo is not valid: %lx", __func__, entryinfo));
#endif

		info = vmcs_read(VMCS_ENTRY_INTR_INFO);
#ifdef	__FreeBSD__
		KASSERT((info & VMCS_INTR_VALID) == 0, ("%s: cannot inject "
		     "pending exception: %#lx/%#x", __func__, entryinfo, info));
#else
		KASSERT((info & VMCS_INTR_VALID) == 0, ("%s: cannot inject "
		     "pending exception: %lx/%x", __func__, entryinfo, info));
#endif

		info = entryinfo;
		vector = info & 0xff;
		if (vector == IDT_BP || vector == IDT_OF) {
			/*
			 * VT-x requires #BP and #OF to be injected as software
			 * exceptions.
			 */
			info &= ~VMCS_INTR_T_MASK;
			info |= VMCS_INTR_T_SWEXCEPTION;
		}

		if (info & VMCS_INTR_DEL_ERRCODE)
			vmcs_write(VMCS_ENTRY_EXCEPTION_ERROR, entryinfo >> 32);

		vmcs_write(VMCS_ENTRY_INTR_INFO, info);
	}

	if (vm_nmi_pending(vmx->vm, vcpu)) {
		/*
		 * If there are no conditions blocking NMI injection then
		 * inject it directly here otherwise enable "NMI window
		 * exiting" to inject it as soon as we can.
		 *
		 * We also check for STI_BLOCKING because some implementations
		 * don't allow NMI injection in this case. If we are running
		 * on a processor that doesn't have this restriction it will
		 * immediately exit and the NMI will be injected in the
		 * "NMI window exiting" handler.
		 */
		need_nmi_exiting = 1;
		gi = vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
		if ((gi & (HWINTR_BLOCKING | NMI_BLOCKING)) == 0) {
			info = vmcs_read(VMCS_ENTRY_INTR_INFO);
			if ((info & VMCS_INTR_VALID) == 0) {
				vmx_inject_nmi(vmx, vcpu);
				need_nmi_exiting = 0;
			} else {
				VCPU_CTR1(vmx->vm, vcpu, "Cannot inject NMI "
				    "due to VM-entry intr info %#x", info);
			}
		} else {
			VCPU_CTR1(vmx->vm, vcpu, "Cannot inject NMI due to "
			    "Guest Interruptibility-state %#x", gi);
		}

		if (need_nmi_exiting)
			vmx_set_nmi_window_exiting(vmx, vcpu);
	}

	extint_pending = vm_extint_pending(vmx->vm, vcpu);

#ifdef	__FreeBSD__
	if (!extint_pending && virtual_interrupt_delivery) {
		vmx_inject_pir(vlapic);
		return;
	}
#endif

	/*
	 * If interrupt-window exiting is already in effect then don't bother
	 * checking for pending interrupts. This is just an optimization and
	 * not needed for correctness.
	 */
	if ((vmx->cap[vcpu].proc_ctls & PROCBASED_INT_WINDOW_EXITING) != 0) {
		VCPU_CTR0(vmx->vm, vcpu, "Skip interrupt injection due to "
		    "pending int_window_exiting");
		return;
	}

	if (!extint_pending) {
		/* Ask the local apic for a vector to inject */
		if (!vlapic_pending_intr(vlapic, &vector))
			return;

		/*
		 * From the Intel SDM, Volume 3, Section "Maskable
		 * Hardware Interrupts":
		 * - maskable interrupt vectors [16,255] can be delivered
		 *   through the local APIC.
		*/
		KASSERT(vector >= 16 && vector <= 255,
		    ("invalid vector %d from local APIC", vector));
	} else {
		/* Ask the legacy pic for a vector to inject */
		vatpic_pending_intr(vmx->vm, &vector);

		/*
		 * From the Intel SDM, Volume 3, Section "Maskable
		 * Hardware Interrupts":
		 * - maskable interrupt vectors [0,255] can be delivered
		 *   through the INTR pin.
		 */
		KASSERT(vector >= 0 && vector <= 255,
		    ("invalid vector %d from INTR", vector));
	}

	/* Check RFLAGS.IF and the interruptibility state of the guest */
	rflags = vmcs_read(VMCS_GUEST_RFLAGS);
	if ((rflags & PSL_I) == 0) {
		VCPU_CTR2(vmx->vm, vcpu, "Cannot inject vector %d due to "
		    "rflags %#lx", vector, rflags);
		goto cantinject;
	}

	gi = vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
	if (gi & HWINTR_BLOCKING) {
		VCPU_CTR2(vmx->vm, vcpu, "Cannot inject vector %d due to "
		    "Guest Interruptibility-state %#x", vector, gi);
		goto cantinject;
	}

	info = vmcs_read(VMCS_ENTRY_INTR_INFO);
	if (info & VMCS_INTR_VALID) {
		/*
		 * This is expected and could happen for multiple reasons:
		 * - A vectoring VM-entry was aborted due to astpending
		 * - A VM-exit happened during event injection.
		 * - An exception was injected above.
		 * - An NMI was injected above or after "NMI window exiting"
		 */
		VCPU_CTR2(vmx->vm, vcpu, "Cannot inject vector %d due to "
		    "VM-entry intr info %#x", vector, info);
		goto cantinject;
	}

	/* Inject the interrupt */
	info = VMCS_INTR_T_HWINTR | VMCS_INTR_VALID;
	info |= vector;
	vmcs_write(VMCS_ENTRY_INTR_INFO, info);

	if (!extint_pending) {
		/* Update the Local APIC ISR */
		vlapic_intr_accepted(vlapic, vector);
	} else {
		vm_extint_clear(vmx->vm, vcpu);
		vatpic_intr_accepted(vmx->vm, vector);

		/*
		 * After we accepted the current ExtINT the PIC may
		 * have posted another one.  If that is the case, set
		 * the Interrupt Window Exiting execution control so
		 * we can inject that one too.
		 *
		 * Also, interrupt window exiting allows us to inject any
		 * pending APIC vector that was preempted by the ExtINT
		 * as soon as possible. This applies both for the software
		 * emulated vlapic and the hardware assisted virtual APIC.
		 */
		vmx_set_int_window_exiting(vmx, vcpu);
	}

	VCPU_CTR1(vmx->vm, vcpu, "Injecting hwintr at vector %d", vector);

	return;

cantinject:
	/*
	 * Set the Interrupt Window Exiting execution control so we can inject
	 * the interrupt as soon as blocking condition goes away.
	 */
	vmx_set_int_window_exiting(vmx, vcpu);
}

/*
 * If the Virtual NMIs execution control is '1' then the logical processor
 * tracks virtual-NMI blocking in the Guest Interruptibility-state field of
 * the VMCS. An IRET instruction in VMX non-root operation will remove any
 * virtual-NMI blocking.
 *
 * This unblocking occurs even if the IRET causes a fault. In this case the
 * hypervisor needs to restore virtual-NMI blocking before resuming the guest.
 */
static void
vmx_restore_nmi_blocking(struct vmx *vmx, int vcpuid)
{
	uint32_t gi;

	VCPU_CTR0(vmx->vm, vcpuid, "Restore Virtual-NMI blocking");
	gi = vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
	gi |= VMCS_INTERRUPTIBILITY_NMI_BLOCKING;
	vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, gi);
}

static void
vmx_clear_nmi_blocking(struct vmx *vmx, int vcpuid)
{
	uint32_t gi;

	VCPU_CTR0(vmx->vm, vcpuid, "Clear Virtual-NMI blocking");
	gi = vmcs_read(VMCS_GUEST_INTERRUPTIBILITY);
	gi &= ~VMCS_INTERRUPTIBILITY_NMI_BLOCKING;
	vmcs_write(VMCS_GUEST_INTERRUPTIBILITY, gi);
}

static uint64_t
vmx_get_guest_reg(struct vmx *vmx, int vcpu, int ident)
{
	const struct vmxctx *vmxctx;

	vmxctx = &vmx->ctx[vcpu];

	switch (ident) {
	case 0:
		return (vmxctx->guest_rax);
	case 1:
		return (vmxctx->guest_rcx);
	case 2:
		return (vmxctx->guest_rdx);
	case 3:
		return (vmxctx->guest_rbx);
	case 4:
		return (vmcs_read(VMCS_GUEST_RSP));
	case 5:
		return (vmxctx->guest_rbp);
	case 6:
		return (vmxctx->guest_rsi);
	case 7:
		return (vmxctx->guest_rdi);
	case 8:
		return (vmxctx->guest_r8);
	case 9:
		return (vmxctx->guest_r9);
	case 10:
		return (vmxctx->guest_r10);
	case 11:
		return (vmxctx->guest_r11);
	case 12:
		return (vmxctx->guest_r12);
	case 13:
		return (vmxctx->guest_r13);
	case 14:
		return (vmxctx->guest_r14);
	case 15:
		return (vmxctx->guest_r15);
	default:
		panic("invalid vmx register %d", ident);
	}
}

static void
vmx_set_guest_reg(struct vmx *vmx, int vcpu, int ident, uint64_t regval)
{
	struct vmxctx *vmxctx;

	vmxctx = &vmx->ctx[vcpu];

	switch (ident) {
	case 0:
		vmxctx->guest_rax = regval;
		break;
	case 1:
		vmxctx->guest_rcx = regval;
		break;
	case 2:
		vmxctx->guest_rdx = regval;
		break;
	case 3:
		vmxctx->guest_rbx = regval;
		break;
	case 4:
		vmcs_write(VMCS_GUEST_RSP, regval);
		break;
	case 5:
		vmxctx->guest_rbp = regval;
		break;
	case 6:
		vmxctx->guest_rsi = regval;
		break;
	case 7:
		vmxctx->guest_rdi = regval;
		break;
	case 8:
		vmxctx->guest_r8 = regval;
		break;
	case 9:
		vmxctx->guest_r9 = regval;
		break;
	case 10:
		vmxctx->guest_r10 = regval;
		break;
	case 11:
		vmxctx->guest_r11 = regval;
		break;
	case 12:
		vmxctx->guest_r12 = regval;
		break;
	case 13:
		vmxctx->guest_r13 = regval;
		break;
	case 14:
		vmxctx->guest_r14 = regval;
		break;
	case 15:
		vmxctx->guest_r15 = regval;
		break;
	default:
		panic("invalid vmx register %d", ident);
	}
}

static int
vmx_emulate_cr0_access(struct vmx *vmx, int vcpu, uint64_t exitqual)
{
	uint64_t crval, regval;

	/* We only handle mov to %cr0 at this time */
	if ((exitqual & 0xf0) != 0x00)
		return (UNHANDLED);

	regval = vmx_get_guest_reg(vmx, vcpu, (exitqual >> 8) & 0xf);

	vmcs_write(VMCS_CR0_SHADOW, regval);

	crval = regval | cr0_ones_mask;
	crval &= ~cr0_zeros_mask;
	vmcs_write(VMCS_GUEST_CR0, crval);

	if (regval & CR0_PG) {
		uint64_t efer, entry_ctls;

		/*
		 * If CR0.PG is 1 and EFER.LME is 1 then EFER.LMA and
		 * the "IA-32e mode guest" bit in VM-entry control must be
		 * equal.
		 */
		efer = vmcs_read(VMCS_GUEST_IA32_EFER);
		if (efer & EFER_LME) {
			efer |= EFER_LMA;
			vmcs_write(VMCS_GUEST_IA32_EFER, efer);
			entry_ctls = vmcs_read(VMCS_ENTRY_CTLS);
			entry_ctls |= VM_ENTRY_GUEST_LMA;
			vmcs_write(VMCS_ENTRY_CTLS, entry_ctls);
		}
	}

	return (HANDLED);
}

static int
vmx_emulate_cr4_access(struct vmx *vmx, int vcpu, uint64_t exitqual)
{
	uint64_t crval, regval;

	/* We only handle mov to %cr4 at this time */
	if ((exitqual & 0xf0) != 0x00)
		return (UNHANDLED);

	regval = vmx_get_guest_reg(vmx, vcpu, (exitqual >> 8) & 0xf);

	vmcs_write(VMCS_CR4_SHADOW, regval);

	crval = regval | cr4_ones_mask;
	crval &= ~cr4_zeros_mask;
	vmcs_write(VMCS_GUEST_CR4, crval);

	return (HANDLED);
}

static int
vmx_emulate_cr8_access(struct vmx *vmx, int vcpu, uint64_t exitqual)
{
	struct vlapic *vlapic;
	uint64_t cr8;
	int regnum;

	/* We only handle mov %cr8 to/from a register at this time. */
	if ((exitqual & 0xe0) != 0x00) {
		return (UNHANDLED);
	}

	vlapic = vm_lapic(vmx->vm, vcpu);
	regnum = (exitqual >> 8) & 0xf;
	if (exitqual & 0x10) {
		cr8 = vlapic_get_cr8(vlapic);
		vmx_set_guest_reg(vmx, vcpu, regnum, cr8);
	} else {
		cr8 = vmx_get_guest_reg(vmx, vcpu, regnum);
		vlapic_set_cr8(vlapic, cr8);
	}

	return (HANDLED);
}

/*
 * From section "Guest Register State" in the Intel SDM: CPL = SS.DPL
 */
static int
vmx_cpl(void)
{
	uint32_t ssar;

	ssar = vmcs_read(VMCS_GUEST_SS_ACCESS_RIGHTS);
	return ((ssar >> 5) & 0x3);
}

static enum vm_cpu_mode
vmx_cpu_mode(void)
{
	uint32_t csar;

	if (vmcs_read(VMCS_GUEST_IA32_EFER) & EFER_LMA) {
		csar = vmcs_read(VMCS_GUEST_CS_ACCESS_RIGHTS);
		if (csar & 0x2000)
			return (CPU_MODE_64BIT);	/* CS.L = 1 */
		else
			return (CPU_MODE_COMPATIBILITY);
	} else if (vmcs_read(VMCS_GUEST_CR0) & CR0_PE) {
		return (CPU_MODE_PROTECTED);
	} else {
		return (CPU_MODE_REAL);
	}
}

static enum vm_paging_mode
vmx_paging_mode(void)
{

	if (!(vmcs_read(VMCS_GUEST_CR0) & CR0_PG))
		return (PAGING_MODE_FLAT);
	if (!(vmcs_read(VMCS_GUEST_CR4) & CR4_PAE))
		return (PAGING_MODE_32);
	if (vmcs_read(VMCS_GUEST_IA32_EFER) & EFER_LME)
		return (PAGING_MODE_64);
	else
		return (PAGING_MODE_PAE);
}

static uint64_t
inout_str_index(struct vmx *vmx, int vcpuid, int in)
{
	uint64_t val;
	int error;
	enum vm_reg_name reg;

	reg = in ? VM_REG_GUEST_RDI : VM_REG_GUEST_RSI;
	error = vmx_getreg(vmx, vcpuid, reg, &val);
	KASSERT(error == 0, ("%s: vmx_getreg error %d", __func__, error));
	return (val);
}

static uint64_t
inout_str_count(struct vmx *vmx, int vcpuid, int rep)
{
	uint64_t val;
	int error;

	if (rep) {
		error = vmx_getreg(vmx, vcpuid, VM_REG_GUEST_RCX, &val);
		KASSERT(!error, ("%s: vmx_getreg error %d", __func__, error));
	} else {
		val = 1;
	}
	return (val);
}

static int
inout_str_addrsize(uint32_t inst_info)
{
	uint32_t size;

	size = (inst_info >> 7) & 0x7;
	switch (size) {
	case 0:
		return (2);	/* 16 bit */
	case 1:
		return (4);	/* 32 bit */
	case 2:
		return (8);	/* 64 bit */
	default:
		panic("%s: invalid size encoding %d", __func__, size);
	}
}

static void
inout_str_seginfo(struct vmx *vmx, int vcpuid, uint32_t inst_info, int in,
    struct vm_inout_str *vis)
{
	int error, s;

	if (in) {
		vis->seg_name = VM_REG_GUEST_ES;
	} else {
		s = (inst_info >> 15) & 0x7;
		vis->seg_name = vm_segment_name(s);
	}

	error = vmx_getdesc(vmx, vcpuid, vis->seg_name, &vis->seg_desc);
	KASSERT(error == 0, ("%s: vmx_getdesc error %d", __func__, error));
}

static void
vmx_paging_info(struct vm_guest_paging *paging)
{
	paging->cr3 = vmcs_guest_cr3();
	paging->cpl = vmx_cpl();
	paging->cpu_mode = vmx_cpu_mode();
	paging->paging_mode = vmx_paging_mode();
}

static void
vmexit_inst_emul(struct vm_exit *vmexit, uint64_t gpa, uint64_t gla)
{
	struct vm_guest_paging *paging;
	uint32_t csar;

	paging = &vmexit->u.inst_emul.paging;

	vmexit->exitcode = VM_EXITCODE_INST_EMUL;
	vmexit->u.inst_emul.gpa = gpa;
	vmexit->u.inst_emul.gla = gla;
	vmx_paging_info(paging);
	switch (paging->cpu_mode) {
	case CPU_MODE_REAL:
		vmexit->u.inst_emul.cs_base = vmcs_read(VMCS_GUEST_CS_BASE);
		vmexit->u.inst_emul.cs_d = 0;
		break;
	case CPU_MODE_PROTECTED:
	case CPU_MODE_COMPATIBILITY:
		vmexit->u.inst_emul.cs_base = vmcs_read(VMCS_GUEST_CS_BASE);
		csar = vmcs_read(VMCS_GUEST_CS_ACCESS_RIGHTS);
		vmexit->u.inst_emul.cs_d = SEG_DESC_DEF32(csar);
		break;
	default:
		vmexit->u.inst_emul.cs_base = 0;
		vmexit->u.inst_emul.cs_d = 0;
		break;
	}
	vie_init(&vmexit->u.inst_emul.vie, NULL, 0);
}

static int
ept_fault_type(uint64_t ept_qual)
{
	int fault_type;

	if (ept_qual & EPT_VIOLATION_DATA_WRITE)
		fault_type = VM_PROT_WRITE;
	else if (ept_qual & EPT_VIOLATION_INST_FETCH)
		fault_type = VM_PROT_EXECUTE;
	else
		fault_type= VM_PROT_READ;

	return (fault_type);
}

static boolean_t
ept_emulation_fault(uint64_t ept_qual)
{
	int read, write;

	/* EPT fault on an instruction fetch doesn't make sense here */
	if (ept_qual & EPT_VIOLATION_INST_FETCH)
		return (FALSE);

	/* EPT fault must be a read fault or a write fault */
	read = ept_qual & EPT_VIOLATION_DATA_READ ? 1 : 0;
	write = ept_qual & EPT_VIOLATION_DATA_WRITE ? 1 : 0;
	if ((read | write) == 0)
		return (FALSE);

	/*
	 * The EPT violation must have been caused by accessing a
	 * guest-physical address that is a translation of a guest-linear
	 * address.
	 */
	if ((ept_qual & EPT_VIOLATION_GLA_VALID) == 0 ||
	    (ept_qual & EPT_VIOLATION_XLAT_VALID) == 0) {
		return (FALSE);
	}

	return (TRUE);
}

static int
emulate_wrmsr(struct vmx *vmx, int vcpuid, u_int num, uint64_t val, bool *retu)
{
	int error;

	if (lapic_msr(num))
		error = lapic_wrmsr(vmx->vm, vcpuid, num, val, retu);
	else
		error = vmx_wrmsr(vmx, vcpuid, num, val, retu);

	return (error);
}

static int
emulate_rdmsr(struct vmx *vmx, int vcpuid, u_int num, bool *retu)
{
	struct vmxctx *vmxctx;
	uint64_t result;
	uint32_t eax, edx;
	int error;

	if (lapic_msr(num))
		error = lapic_rdmsr(vmx->vm, vcpuid, num, &result, retu);
	else
		error = vmx_rdmsr(vmx, vcpuid, num, &result, retu);

	if (error == 0) {
		eax = result;
		vmxctx = &vmx->ctx[vcpuid];
		error = vmxctx_setreg(vmxctx, VM_REG_GUEST_RAX, eax);
		KASSERT(error == 0, ("vmxctx_setreg(rax) error %d", error));

		edx = result >> 32;
		error = vmxctx_setreg(vmxctx, VM_REG_GUEST_RDX, edx);
		KASSERT(error == 0, ("vmxctx_setreg(rdx) error %d", error));
	}

	return (error);
}

static int
vmx_exit_process(struct vmx *vmx, int vcpu, struct vm_exit *vmexit)
{
	int error, handled, in;
	struct vmcs *vmcs;
	struct vmxctx *vmxctx;
	struct vm_inout_str *vis;
	uint32_t eax, ecx, edx, idtvec_info, intr_info, inst_info;
	uint64_t qual, gla, gpa, cr3;
	bool retu;

	CTASSERT((PINBASED_CTLS_ONE_SETTING & PINBASED_VIRTUAL_NMI) != 0);
	CTASSERT((PINBASED_CTLS_ONE_SETTING & PINBASED_NMI_EXITING) != 0);

	handled = UNHANDLED;
	vmcs = &vmx->vmcs[vcpu];
	vmxctx = &vmx->ctx[vcpu];
	qual = vmexit->u.vmx.exit_qualification;
	vmexit->exitcode = VM_EXITCODE_BOGUS;

	vmm_stat_incr(vmx->vm, vcpu, VMEXIT_COUNT, 1);

	switch (vmexit->u.vmx.exit_reason) {
	case EXIT_REASON_CR_ACCESS:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_CR_ACCESS, 1);
		switch (qual & 0xf) {
		case 0:
			handled = vmx_emulate_cr0_access(vmx, vcpu, qual);
			break;
		case 4:
			handled = vmx_emulate_cr4_access(vmx, vcpu, qual);
			break;
		case 8:
			handled = vmx_emulate_cr8_access(vmx, vcpu, qual);
			break;
		}
		break;
	case EXIT_REASON_RDMSR:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_RDMSR, 1);
		retu = false;
		ecx = vmxctx->guest_rcx;
		VCPU_CTR1(vmx->vm, vcpu, "rdmsr 0x%08x", ecx);
		error = emulate_rdmsr(vmx, vcpu, ecx, &retu);
		if (error) {
			vmexit->exitcode = VM_EXITCODE_RDMSR;
			vmexit->u.msr.code = ecx;
		} else if (!retu) {
			handled = HANDLED;
		} else {
			/* Return to userspace with a valid exitcode */
			KASSERT(vmexit->exitcode != VM_EXITCODE_BOGUS,
			    ("emulate_rdmsr retu with bogus exitcode"));
		}
		break;
	case EXIT_REASON_WRMSR:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_WRMSR, 1);
		retu = false;
		eax = vmxctx->guest_rax;
		ecx = vmxctx->guest_rcx;
		edx = vmxctx->guest_rdx;
		VCPU_CTR2(vmx->vm, vcpu, "wrmsr 0x%08x value 0x%016lx",
		    ecx, (uint64_t)edx << 32 | eax);
		error = emulate_wrmsr(vmx, vcpu, ecx,
		    (uint64_t)edx << 32 | eax, &retu);
		if (error) {
			vmexit->exitcode = VM_EXITCODE_WRMSR;
			vmexit->u.msr.code = ecx;
			vmexit->u.msr.wval = (uint64_t)edx << 32 | eax;
		} else if (!retu) {
			handled = HANDLED;
		} else {
			/* Return to userspace with a valid exitcode */
			KASSERT(vmexit->exitcode != VM_EXITCODE_BOGUS,
			    ("emulate_wrmsr retu with bogus exitcode"));
		}
		break;
	case EXIT_REASON_HLT:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_HLT, 1);
		vmexit->exitcode = VM_EXITCODE_HLT;
		vmexit->u.hlt.rflags = vmcs_read(VMCS_GUEST_RFLAGS);
		break;
	case EXIT_REASON_MTF:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_MTRAP, 1);
		vmexit->exitcode = VM_EXITCODE_MTRAP;
		break;
	case EXIT_REASON_PAUSE:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_PAUSE, 1);
		vmexit->exitcode = VM_EXITCODE_PAUSE;
		break;
	case EXIT_REASON_INTR_WINDOW:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_INTR_WINDOW, 1);
		vmx_clear_int_window_exiting(vmx, vcpu);
		return (1);
	case EXIT_REASON_EXT_INTR:
		/*
		 * External interrupts serve only to cause VM exits and allow
		 * the host interrupt handler to run.
		 *
		 * If this external interrupt triggers a virtual interrupt
		 * to a VM, then that state will be recorded by the
		 * host interrupt handler in the VM's softc. We will inject
		 * this virtual interrupt during the subsequent VM enter.
		 */
		intr_info = vmcs_read(VMCS_EXIT_INTR_INFO);

		/*
		 * XXX: Ignore this exit if VMCS_INTR_VALID is not set.
		 * This appears to be a bug in VMware Fusion?
		 */
		if (!(intr_info & VMCS_INTR_VALID))
			return (1);
#ifdef	__FreeBSD__
		KASSERT((intr_info & VMCS_INTR_VALID) != 0 &&
		    (intr_info & VMCS_INTR_T_MASK) == VMCS_INTR_T_HWINTR,
		    ("VM exit interruption info invalid: %#x", intr_info));
#else
		KASSERT((intr_info & VMCS_INTR_VALID) != 0 &&
		    (intr_info & VMCS_INTR_T_MASK) == VMCS_INTR_T_HWINTR,
		    ("VM exit interruption info invalid: %x", intr_info));
#endif
#if 0	/* XXX */
		vmx_trigger_hostintr(intr_info & 0xff);
#endif

		/*
		 * This is special. We want to treat this as an 'handled'
		 * VM-exit but not increment the instruction pointer.
		 */
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_EXTINT, 1);
		return (1);
	case EXIT_REASON_NMI_WINDOW:
		/* Exit to allow the pending virtual NMI to be injected */
		if (vm_nmi_pending(vmx->vm, vcpu))
			vmx_inject_nmi(vmx, vcpu);
		vmx_clear_nmi_window_exiting(vmx, vcpu);
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_NMI_WINDOW, 1);
		return (1);
	case EXIT_REASON_INOUT:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_INOUT, 1);
		vmexit->exitcode = VM_EXITCODE_INOUT;
		vmexit->u.inout.bytes = (qual & 0x7) + 1;
		vmexit->u.inout.in = in = (qual & 0x8) ? 1 : 0;
		vmexit->u.inout.string = (qual & 0x10) ? 1 : 0;
		vmexit->u.inout.rep = (qual & 0x20) ? 1 : 0;
		vmexit->u.inout.port = (uint16_t)(qual >> 16);
		vmexit->u.inout.eax = (uint32_t)(vmxctx->guest_rax);
		if (vmexit->u.inout.string) {
			inst_info = vmcs_read(VMCS_EXIT_INSTRUCTION_INFO);
			vmexit->exitcode = VM_EXITCODE_INOUT_STR;
			vis = &vmexit->u.inout_str;
			vmx_paging_info(&vis->paging);
			vis->rflags = vmcs_read(VMCS_GUEST_RFLAGS);
			vis->cr0 = vmcs_read(VMCS_GUEST_CR0);
			vis->index = inout_str_index(vmx, vcpu, in);
			vis->count = inout_str_count(vmx, vcpu, vis->inout.rep);
			vis->addrsize = inout_str_addrsize(inst_info);
			inout_str_seginfo(vmx, vcpu, inst_info, in, vis);
		}
		break;
	case EXIT_REASON_CPUID:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_CPUID, 1);
		handled = vmx_handle_cpuid(vmx->vm, vcpu, vmxctx);
		break;
	case EXIT_REASON_EXCEPTION:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_EXCEPTION, 1);
		intr_info = vmcs_read(VMCS_EXIT_INTR_INFO);
#ifdef	__FreeBSD__
		KASSERT((intr_info & VMCS_INTR_VALID) != 0,
		    ("VM exit interruption info invalid: %#x", intr_info));
#else
		KASSERT((intr_info & VMCS_INTR_VALID) != 0,
		    ("VM exit interruption info invalid: %x", intr_info));
#endif

		/*
		 * If Virtual NMIs control is 1 and the VM-exit is due to a
		 * fault encountered during the execution of IRET then we must
		 * restore the state of "virtual-NMI blocking" before resuming
		 * the guest.
		 *
		 * See "Resuming Guest Software after Handling an Exception".
		 */
		if ((idtvec_info & VMCS_IDT_VEC_VALID) == 0 &&
		    (intr_info & 0xff) != IDT_DF &&
		    (intr_info & EXIT_QUAL_NMIUDTI) != 0)
			vmx_restore_nmi_blocking(vmx, vcpu);

		/*
		 * The NMI has already been handled in vmx_exit_handle_nmi().
		 */
		if ((intr_info & VMCS_INTR_T_MASK) == VMCS_INTR_T_NMI)
			return (1);
		break;
	case EXIT_REASON_EPT_FAULT:
		gpa = vmcs_gpa();
		if (ept_emulation_fault(qual)) {
			vmexit_inst_emul(vmexit, gpa, vmcs_gla());
			vmm_stat_incr(vmx->vm, vcpu, VMEXIT_INST_EMUL, 1);
		}
		break;
	default:
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_UNKNOWN, 1);
		break;
	}

	if (handled) {
		/*
		 * It is possible that control is returned to userland
		 * even though we were able to handle the VM exit in the
		 * kernel.
		 *
		 * In such a case we want to make sure that the userland
		 * restarts guest execution at the instruction *after*
		 * the one we just processed. Therefore we update the
		 * guest rip in the VMCS and in 'vmexit'.
		 */
		vm_exit_update_rip(vmexit);
		vmexit->rip += vmexit->inst_length;
		vmexit->inst_length = 0;

		/*
		 * Special case for spinning up an AP - exit to userspace to
		 * give the controlling process a chance to intercept and
		 * spin up a thread for the AP.
		 */
		if (vmexit->exitcode == VM_EXITCODE_SPINUP_AP)
			handled = 0;
	} else {
		if (vmexit->exitcode == VM_EXITCODE_BOGUS) {
			/*
			 * If this VM exit was not claimed by anybody then
			 * treat it as a generic VMX exit.
			 */
			vmexit->exitcode = VM_EXITCODE_VMX;
			vmexit->u.vmx.status = VM_SUCCESS;
			vmexit->u.vmx.inst_type = 0;
			vmexit->u.vmx.inst_error = 0;
		} else {
			/*
			 * The exitcode and collateral have been populated.
			 * The VM exit will be processed further in userland.
			 */
		}
	}
	return (handled);
}

static int
vmx_run(void *arg, int vcpu, register_t rip)
{
	int error, vie, rc, handled, astpending;
	uint32_t exit_reason;
	struct vmx *vmx;
	struct vm *vm;
	struct vmxctx *vmxctx;
	struct vmcs *vmcs;
	struct vm_exit *vmexit;
	struct vlapic *vlapic;
	
	vmx = arg;
	vm = vmx->vm;
	vmcs = &vmx->vmcs[vcpu];
	vmxctx = &vmx->ctx[vcpu];
	vlapic = vm_lapic(vm, vcpu);
	vmxctx->launched = 0;

	astpending = 0;
	vmexit = vm_exitinfo(vmx->vm, vcpu);

	vmx_msr_guest_enter(vmx, vcpu);

	VMPTRLD(vmcs);

	/*
	 * XXX
	 * We do this every time because we may setup the virtual machine
	 * from a different process than the one that actually runs it.
	 *
	 * If the life of a virtual machine was spent entirely in the context
	 * of a single process we could do this once in vmcs_set_defaults().
	 */
	vmcs_write(VMCS_HOST_CR3, rcr3());

	vmcs_write(VMCS_GUEST_RIP, rip);
	vmx_set_pcpu_defaults(vmx, vcpu);
	do {
		vmx_inject_interrupts(vmx, vcpu, vlapic);
		vmx_run_trace(vmx, vcpu);
		rc = vmx_setjmp(vmxctx);
#ifdef SETJMP_TRACE
		vmx_setjmp_trace(vmx, vcpu, vmxctx, rc);
#endif
		switch (rc) {
		case VMX_RETURN_DIRECT:
			if (vmxctx->launched == 0) {
				vmxctx->launched = 1;
				vmx_launch(vmxctx);
			} else
				vmx_resume(vmxctx);
			panic("vmx_launch/resume should not return");
			break;
		case VMX_RETURN_LONGJMP:
			break;			/* vm exit */
		case VMX_RETURN_AST:
			astpending = 1;
			break;
		case VMX_RETURN_VMRESUME:
			vie = vmcs_instruction_error();
			if (vmxctx->launch_error == VM_FAIL_INVALID ||
			    vie != VMRESUME_WITH_NON_LAUNCHED_VMCS) {
				printf("vmresume error %d vmcs inst error %d\n",
					vmxctx->launch_error, vie);
				goto err_exit;
			}
			vmx_launch(vmxctx);	/* try to launch the guest */
			panic("vmx_launch should not return");
			break;
		case VMX_RETURN_VMLAUNCH:
			vie = vmcs_instruction_error();
#if 1
			printf("vmlaunch error %d vmcs inst error %d\n",
				vmxctx->launch_error, vie);
#endif
			goto err_exit;
		default:
			panic("vmx_setjmp returned %d", rc);
		}
		
		/* collect some basic information for VM exit processing */
		vmexit->rip = rip = vmcs_guest_rip();
		vmexit->inst_length = vmexit_instruction_length();
		vmexit->u.vmx.exit_reason = exit_reason = vmcs_exit_reason();
		vmexit->u.vmx.exit_qualification = vmcs_exit_qualification();

		/* Update 'nextrip' */
		vmx->state[vcpu].nextrip = rip;

		/* enable interrupts */
		enable_intr();

		if (astpending) {
			handled = 1;
			vmexit->inst_length = 0;
			vmexit->exitcode = VM_EXITCODE_BOGUS;
			vmx_astpending_trace(vmx, vcpu, rip);
			vmm_stat_incr(vmx->vm, vcpu, VMEXIT_ASTPENDING, 1);
			break;
		}

		handled = vmx_exit_process(vmx, vcpu, vmexit);
		vmx_exit_trace(vmx, vcpu, rip, exit_reason, handled);

	} while (handled);

	/*
	 * If a VM exit has been handled then the exitcode must be BOGUS
	 * If a VM exit is not handled then the exitcode must not be BOGUS
	 */
	if ((handled && vmexit->exitcode != VM_EXITCODE_BOGUS) ||
	    (!handled && vmexit->exitcode == VM_EXITCODE_BOGUS)) {
		panic("Mismatch between handled (%d) and exitcode (%d)",
		      handled, vmexit->exitcode);
	}

	if (!handled)
		vmm_stat_incr(vmx->vm, vcpu, VMEXIT_USERSPACE, 1);

	VCPU_CTR1(vmx->vm, vcpu, "goto userland: exitcode %d",
	    vmexit->exitcode);

	VMCLEAR(vmcs);
	vmx_msr_guest_exit(vmx, vcpu);

	return (0);

err_exit:
	vmexit->exitcode = VM_EXITCODE_VMX;
	vmexit->u.vmx.exit_reason = (uint32_t)-1;
	vmexit->u.vmx.exit_qualification = (uint32_t)-1;
	vmexit->u.vmx.status = ~0;
	VMCLEAR(vmcs);
	vmx_msr_guest_exit(vmx, vcpu);

	return (ENOEXEC);
}

static void
vmx_vmcleanup(void *arg)
{
	int i, error;
	struct vmx *vmx = arg;

	for (i = 0; i < VM_MAXCPU; i++)
		vpid_free(vmx->state[i].vpid);

	/*
	 * XXXSMP we also need to clear the VMCS active on the other vcpus.
	 */
	error = vmclear(&vmx->vmcs[0]);
	if (error != 0)
		panic("vmx_vmcleanup: vmclear error %d on vcpu 0", error);

	ept_vmcleanup(vmx);
	free(vmx, M_VMX);

	return;
}

static register_t *
vmxctx_regptr(struct vmxctx *vmxctx, int reg)
{

	switch (reg) {
	case VM_REG_GUEST_RAX:
		return (&vmxctx->guest_rax);
	case VM_REG_GUEST_RBX:
		return (&vmxctx->guest_rbx);
	case VM_REG_GUEST_RCX:
		return (&vmxctx->guest_rcx);
	case VM_REG_GUEST_RDX:
		return (&vmxctx->guest_rdx);
	case VM_REG_GUEST_RSI:
		return (&vmxctx->guest_rsi);
	case VM_REG_GUEST_RDI:
		return (&vmxctx->guest_rdi);
	case VM_REG_GUEST_RBP:
		return (&vmxctx->guest_rbp);
	case VM_REG_GUEST_R8:
		return (&vmxctx->guest_r8);
	case VM_REG_GUEST_R9:
		return (&vmxctx->guest_r9);
	case VM_REG_GUEST_R10:
		return (&vmxctx->guest_r10);
	case VM_REG_GUEST_R11:
		return (&vmxctx->guest_r11);
	case VM_REG_GUEST_R12:
		return (&vmxctx->guest_r12);
	case VM_REG_GUEST_R13:
		return (&vmxctx->guest_r13);
	case VM_REG_GUEST_R14:
		return (&vmxctx->guest_r14);
	case VM_REG_GUEST_R15:
		return (&vmxctx->guest_r15);
	case VM_REG_GUEST_CR2:
		return (&vmxctx->guest_cr2);
	default:
		break;
	}
	return (NULL);
}

static int
vmxctx_getreg(struct vmxctx *vmxctx, int reg, uint64_t *retval)
{
	register_t *regp;

	if ((regp = vmxctx_regptr(vmxctx, reg)) != NULL) {
		*retval = *regp;
		return (0);
	} else
		return (EINVAL);
}

static int
vmxctx_setreg(struct vmxctx *vmxctx, int reg, uint64_t val)
{
	register_t *regp;

	if ((regp = vmxctx_regptr(vmxctx, reg)) != NULL) {
		*regp = val;
		return (0);
	} else
		return (EINVAL);
}

static int
vmx_shadow_reg(int reg)
{
	int shreg;

	shreg = -1;

	switch (reg) {
	case VM_REG_GUEST_CR0:
		shreg = VMCS_CR0_SHADOW;
                break;
        case VM_REG_GUEST_CR4:
		shreg = VMCS_CR4_SHADOW;
		break;
	default:
		break;
	}

	return (shreg);
}

static int
vmx_getreg(void *arg, int vcpu, int reg, uint64_t *retval)
{
	int running, hostcpu;
	struct vmx *vmx = arg;

	running = vcpu_is_running(vmx->vm, vcpu, &hostcpu);
	if (running && hostcpu != curcpu)
		panic("vmx_getreg: %s%d is running", vm_name(vmx->vm), vcpu);

	if (vmxctx_getreg(&vmx->ctx[vcpu], reg, retval) == 0)
		return (0);

	return (vmcs_getreg(&vmx->vmcs[vcpu], running, reg, retval));
}

static int
vmx_setreg(void *arg, int vcpu, int reg, uint64_t val)
{
	int error, hostcpu, running, shadow;
	uint64_t ctls;
	struct vmx *vmx = arg;

	running = vcpu_is_running(vmx->vm, vcpu, &hostcpu);
	if (running && hostcpu != curcpu)
		panic("vmx_setreg: %s%d is running", vm_name(vmx->vm), vcpu);

	if (vmxctx_setreg(&vmx->ctx[vcpu], reg, val) == 0)
		return (0);

	error = vmcs_setreg(&vmx->vmcs[vcpu], running, reg, val);

	if (error == 0) {
		/*
		 * If the "load EFER" VM-entry control is 1 then the
		 * value of EFER.LMA must be identical to "IA-32e mode guest"
		 * bit in the VM-entry control.
		 */
		if ((entry_ctls & VM_ENTRY_LOAD_EFER) != 0 &&
		    (reg == VM_REG_GUEST_EFER)) {
			vmcs_getreg(&vmx->vmcs[vcpu], running,
				    VMCS_IDENT(VMCS_ENTRY_CTLS), &ctls);
			if (val & EFER_LMA)
				ctls |= VM_ENTRY_GUEST_LMA;
			else
				ctls &= ~VM_ENTRY_GUEST_LMA;
			vmcs_setreg(&vmx->vmcs[vcpu], running,
				    VMCS_IDENT(VMCS_ENTRY_CTLS), ctls);
		}

		shadow = vmx_shadow_reg(reg);
		if (shadow > 0) {
			/*
			 * Store the unmodified value in the shadow
			 */			
			error = vmcs_setreg(&vmx->vmcs[vcpu], running,
				    VMCS_IDENT(shadow), val);
		}
	}

	return (error);
}

static int
vmx_getdesc(void *arg, int vcpu, int reg, struct seg_desc *desc)
{
	int hostcpu, running;
	struct vmx *vmx = arg;

	running = vcpu_is_running(vmx->vm, vcpu, &hostcpu);
	if (running && hostcpu != curcpu)
		panic("vmx_getdesc: %s%d is running", vm_name(vmx->vm), vcpu);

	return (vmcs_getdesc(&vmx->vmcs[vcpu], running, reg, desc));
}

static int
vmx_setdesc(void *arg, int vcpu, int reg, struct seg_desc *desc)
{
	int hostcpu, running;
	struct vmx *vmx = arg;

	running = vcpu_is_running(vmx->vm, vcpu, &hostcpu);
	if (running && hostcpu != curcpu)
		panic("vmx_setdesc: %s%d is running", vm_name(vmx->vm), vcpu);

	return (vmcs_setdesc(&vmx->vmcs[vcpu], running, reg, desc));
}

static int
vmx_getcap(void *arg, int vcpu, int type, int *retval)
{
	struct vmx *vmx = arg;
	int vcap;
	int ret;

	ret = ENOENT;

	vcap = vmx->cap[vcpu].set;

	switch (type) {
	case VM_CAP_HALT_EXIT:
		if (cap_halt_exit)
			ret = 0;
		break;
	case VM_CAP_PAUSE_EXIT:
		if (cap_pause_exit)
			ret = 0;
		break;
	case VM_CAP_MTRAP_EXIT:
		if (cap_monitor_trap)
			ret = 0;
		break;
	case VM_CAP_UNRESTRICTED_GUEST:
		if (cap_unrestricted_guest)
			ret = 0;
		break;
	default:
		break;
	}

	if (ret == 0)
		*retval = (vcap & (1 << type)) ? 1 : 0;

	return (ret);
}

static int
vmx_setcap(void *arg, int vcpu, int type, int val)
{
	struct vmx *vmx = arg;
	struct vmcs *vmcs = &vmx->vmcs[vcpu];
	uint32_t baseval;
	uint32_t *pptr;
	int error;
	int flag;
	int reg;
	int retval;

	retval = ENOENT;
	pptr = NULL;

	switch (type) {
	case VM_CAP_HALT_EXIT:
		if (cap_halt_exit) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_HLT_EXITING;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_MTRAP_EXIT:
		if (cap_monitor_trap) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_MTF;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_PAUSE_EXIT:
		if (cap_pause_exit) {
			retval = 0;
			pptr = &vmx->cap[vcpu].proc_ctls;
			baseval = *pptr;
			flag = PROCBASED_PAUSE_EXITING;
			reg = VMCS_PRI_PROC_BASED_CTLS;
		}
		break;
	case VM_CAP_UNRESTRICTED_GUEST:
		if (cap_unrestricted_guest) {
			retval = 0;
			baseval = procbased_ctls2;
			flag = PROCBASED2_UNRESTRICTED_GUEST;
			reg = VMCS_SEC_PROC_BASED_CTLS;
		}
		break;
	default:
		break;
	}

	if (retval == 0) {
		if (val) {
			baseval |= flag;
		} else {
			baseval &= ~flag;
		}
		VMPTRLD(vmcs);
		error = vmwrite(reg, baseval);
		VMCLEAR(vmcs);

		if (error) {
			retval = error;
		} else {
			/*
			 * Update optional stored flags, and record
			 * setting
			 */
			if (pptr != NULL) {
				*pptr = baseval;
			}

			if (val) {
				vmx->cap[vcpu].set |= (1 << type);
			} else {
				vmx->cap[vcpu].set &= ~(1 << type);
			}
		}
	}

        return (retval);
}

struct vlapic_vtx {
	struct vlapic	vlapic;
	struct pir_desc	*pir_desc;
	struct vmx	*vmx;
};

#define	VMX_CTR_PIR(vm, vcpuid, pir_desc, notify, vector, level, msg)	\
do {									\
	VCPU_CTR2(vm, vcpuid, msg " assert %s-triggered vector %d",	\
	    level ? "level" : "edge", vector);				\
	VCPU_CTR1(vm, vcpuid, msg " pir0 0x%016lx", pir_desc->pir[0]);	\
	VCPU_CTR1(vm, vcpuid, msg " pir1 0x%016lx", pir_desc->pir[1]);	\
	VCPU_CTR1(vm, vcpuid, msg " pir2 0x%016lx", pir_desc->pir[2]);	\
	VCPU_CTR1(vm, vcpuid, msg " pir3 0x%016lx", pir_desc->pir[3]);	\
	VCPU_CTR1(vm, vcpuid, msg " notify: %s", notify ? "yes" : "no");\
} while (0)

/*
 * vlapic->ops handlers that utilize the APICv hardware assist described in
 * Chapter 29 of the Intel SDM.
 */
static int
vmx_set_intr_ready(struct vlapic *vlapic, int vector, bool level)
{
	struct vlapic_vtx *vlapic_vtx;
	struct pir_desc *pir_desc;
	uint64_t mask;
	int idx, notify;

	vlapic_vtx = (struct vlapic_vtx *)vlapic;
	pir_desc = vlapic_vtx->pir_desc;

	/*
	 * Keep track of interrupt requests in the PIR descriptor. This is
	 * because the virtual APIC page pointed to by the VMCS cannot be
	 * modified if the vcpu is running.
	 */
	idx = vector / 64;
	mask = 1UL << (vector % 64);
	atomic_set_long(&pir_desc->pir[idx], mask);
	notify = atomic_cmpset_long(&pir_desc->pending, 0, 1);

	VMX_CTR_PIR(vlapic->vm, vlapic->vcpuid, pir_desc, notify, vector,
	    level, "vmx_set_intr_ready");
	return (notify);
}

static int
vmx_pending_intr(struct vlapic *vlapic, int *vecptr)
{
	struct vlapic_vtx *vlapic_vtx;
	struct pir_desc *pir_desc;
	struct LAPIC *lapic;
	uint64_t pending, pirval;
	uint32_t ppr, vpr;
	int i;

	/*
	 * This function is only expected to be called from the 'HLT' exit
	 * handler which does not care about the vector that is pending.
	 */
	KASSERT(vecptr == NULL, ("vmx_pending_intr: vecptr must be NULL"));

	vlapic_vtx = (struct vlapic_vtx *)vlapic;
	pir_desc = vlapic_vtx->pir_desc;

	pending = atomic_load_acq_long(&pir_desc->pending);
	if (!pending)
		return (0);	/* common case */

	/*
	 * If there is an interrupt pending then it will be recognized only
	 * if its priority is greater than the processor priority.
	 *
	 * Special case: if the processor priority is zero then any pending
	 * interrupt will be recognized.
	 */
	lapic = vlapic->apic_page;
	ppr = lapic->ppr & 0xf0;
	if (ppr == 0)
		return (1);

	VCPU_CTR1(vlapic->vm, vlapic->vcpuid, "HLT with non-zero PPR %d",
	    lapic->ppr);

	for (i = 3; i >= 0; i--) {
		pirval = pir_desc->pir[i];
		if (pirval != 0) {
			vpr = (i * 64 + flsl(pirval) - 1) & 0xf0;
			return (vpr > ppr);
		}
	}
	return (0);
}

static void
vmx_intr_accepted(struct vlapic *vlapic, int vector)
{

	panic("vmx_intr_accepted: not expected to be called");
}

static void
vmx_set_tmr(struct vlapic *vlapic, int vector, bool level)
{
	struct vlapic_vtx *vlapic_vtx;
	struct vmx *vmx;
	struct vmcs *vmcs;
	uint64_t mask, val;

	KASSERT(vector >= 0 && vector <= 255, ("invalid vector %d", vector));
	KASSERT(!vcpu_is_running(vlapic->vm, vlapic->vcpuid, NULL),
	    ("vmx_set_tmr: vcpu cannot be running"));

	vlapic_vtx = (struct vlapic_vtx *)vlapic;
	vmx = vlapic_vtx->vmx;
	vmcs = &vmx->vmcs[vlapic->vcpuid];
	mask = 1UL << (vector % 64);

	VMPTRLD(vmcs);
	val = vmcs_read(VMCS_EOI_EXIT(vector));
	if (level)
		val |= mask;
	else
		val &= ~mask;
	vmcs_write(VMCS_EOI_EXIT(vector), val);
	VMCLEAR(vmcs);
}

static void
vmx_post_intr(struct vlapic *vlapic, int hostcpu)
{

	ipi_cpu(hostcpu, pirvec);
}

/*
 * Transfer the pending interrupts in the PIR descriptor to the IRR
 * in the virtual APIC page.
 */
static void
vmx_inject_pir(struct vlapic *vlapic)
{
	struct vlapic_vtx *vlapic_vtx;
	struct pir_desc *pir_desc;
	struct LAPIC *lapic;
	uint64_t val, pirval;
	int rvi, pirbase = -1;
	uint16_t intr_status_old, intr_status_new;

	vlapic_vtx = (struct vlapic_vtx *)vlapic;
	pir_desc = vlapic_vtx->pir_desc;
	if (atomic_cmpset_long(&pir_desc->pending, 1, 0) == 0) {
		VCPU_CTR0(vlapic->vm, vlapic->vcpuid, "vmx_inject_pir: "
		    "no posted interrupt pending");
		return;
	}

	pirval = 0;
	pirbase = -1;
	lapic = vlapic->apic_page;

	val = atomic_readandclear_long(&pir_desc->pir[0]);
	if (val != 0) {
		lapic->irr0 |= val;
		lapic->irr1 |= val >> 32;
		pirbase = 0;
		pirval = val;
	}

	val = atomic_readandclear_long(&pir_desc->pir[1]);
	if (val != 0) {
		lapic->irr2 |= val;
		lapic->irr3 |= val >> 32;
		pirbase = 64;
		pirval = val;
	}

	val = atomic_readandclear_long(&pir_desc->pir[2]);
	if (val != 0) {
		lapic->irr4 |= val;
		lapic->irr5 |= val >> 32;
		pirbase = 128;
		pirval = val;
	}

	val = atomic_readandclear_long(&pir_desc->pir[3]);
	if (val != 0) {
		lapic->irr6 |= val;
		lapic->irr7 |= val >> 32;
		pirbase = 192;
		pirval = val;
	}

	VLAPIC_CTR_IRR(vlapic, "vmx_inject_pir");

	/*
	 * Update RVI so the processor can evaluate pending virtual
	 * interrupts on VM-entry.
	 *
	 * It is possible for pirval to be 0 here, even though the
	 * pending bit has been set. The scenario is:
	 * CPU-Y is sending a posted interrupt to CPU-X, which
	 * is running a guest and processing posted interrupts in h/w.
	 * CPU-X will eventually exit and the state seen in s/w is
	 * the pending bit set, but no PIR bits set.
	 *
	 *      CPU-X                      CPU-Y
	 *   (vm running)                (host running)
	 *   rx posted interrupt
	 *   CLEAR pending bit
	 *				 SET PIR bit
	 *   READ/CLEAR PIR bits
	 *				 SET pending bit
	 *   (vm exit)
	 *   pending bit set, PIR 0
	 */
	if (pirval != 0) {
		rvi = pirbase + flsl(pirval) - 1;
		intr_status_old = vmcs_read(VMCS_GUEST_INTR_STATUS);
		intr_status_new = (intr_status_old & 0xFF00) | rvi;
		if (intr_status_new > intr_status_old) {
			vmcs_write(VMCS_GUEST_INTR_STATUS, intr_status_new);
			VCPU_CTR2(vlapic->vm, vlapic->vcpuid, "vmx_inject_pir: "
			    "guest_intr_status changed from 0x%04x to 0x%04x",
			    intr_status_old, intr_status_new);
		}
	}
}

static struct vlapic *
vmx_vlapic_init(void *arg, int vcpuid)
{
	struct vmx *vmx;
	struct vlapic *vlapic;
	struct vlapic_vtx *vlapic_vtx;
	
	vmx = arg;

	vlapic = malloc(sizeof(struct vlapic_vtx), M_VLAPIC, M_WAITOK | M_ZERO);
	vlapic->vm = vmx->vm;
	vlapic->vcpuid = vcpuid;
	vlapic->apic_page = (struct LAPIC *)&vmx->apic_page[vcpuid];

	vlapic_vtx = (struct vlapic_vtx *)vlapic;
	vlapic_vtx->pir_desc = &vmx->pir_desc[vcpuid];
	vlapic_vtx->vmx = vmx;

	if (virtual_interrupt_delivery) {
		vlapic->ops.set_intr_ready = vmx_set_intr_ready;
		vlapic->ops.pending_intr = vmx_pending_intr;
		vlapic->ops.intr_accepted = vmx_intr_accepted;
		vlapic->ops.set_tmr = vmx_set_tmr;
#ifdef	__FreeBSD__
		vlapic->ops.enable_x2apic_mode = vmx_enable_x2apic_mode;
#endif
	}

	if (posted_interrupts)
		vlapic->ops.post_intr = vmx_post_intr;

	vlapic_init(vlapic);

	return (vlapic);
}

static void
vmx_vlapic_cleanup(void *arg, struct vlapic *vlapic)
{

	vlapic_cleanup(vlapic);
	free(vlapic, M_VLAPIC);
}

struct vmm_ops vmm_ops_intel = {
	vmx_init,
	vmx_cleanup,
	vmx_vminit,
	vmx_run,
	vmx_vmcleanup,
	ept_vmmmap_set,
	ept_vmmmap_get,
	vmx_getreg,
	vmx_setreg,
	vmx_getdesc,
	vmx_setdesc,
	vmx_getcap,
	vmx_setcap,
	vmx_vlapic_init,
	vmx_vlapic_cleanup,
};