// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt 

#include <linux/export.h>
#include <linux/reboot.h>
#include <linux/init.h>
#include <linux/pm.h>
#include <linux/efi.h>
#include <linux/dmi.h>
#include <linux/sched.h>
#include <linux/tboot.h>
#include <linux/delay.h>
#include <linux/objtool.h>
#include <linux/pgtable.h>
#include <linux/kexec.h>
#include <linux/kvm_types.h>
#include <acpi/reboot.h>
#include <asm/io.h>
#include <asm/apic.h>
#include <asm/io_apic.h>
#include <asm/desc.h>
#include <asm/hpet.h>
#include <asm/proto.h>
#include <asm/reboot_fixups.h>
#include <asm/reboot.h>
#include <asm/pci_x86.h>
#include <asm/cpu.h>
#include <asm/nmi.h>
#include <asm/smp.h>
#include <asm/virt.h>

#include <linux/ctype.h>
#include <linux/mc146818rtc.h>
#include <asm/realmode.h>
#include <asm/x86_init.h>
#include <asm/efi.h>

/*
 * Power off function, if any
 */
void (*pm_power_off) (void);
EXPORT_SYMBOL(pm_power_off);

/*
 * This is set if we need to go through the 'emergency' path.
 * When machine_emergency_restart() is called, we may be on
 * an inconsistent state and won't be able to do a clean cleanup
 */
static int reboot_emergency;

/* This is set by the PCI code if either type 1 or type 2 PCI is detected */
bool port_cf9_safe = false;

/*
 * Reboot options and system auto-detection code provided by
 * Dell Inc. so their systems "just work". :-)
 */

/*
 * Some machines require the "reboot=a" commandline options
 */
static int __init set_acpi_reboot(const struct dmi_system_id *d)
{
        if (reboot_type != BOOT_ACPI) {
                reboot_type = BOOT_ACPI;
                pr_info("%s series board detected. Selecting %s-method for reboots.\n",
                        d->ident, "ACPI");
        }
        return 0;
}

/*
 * Some machines require the "reboot=b" or "reboot=k"  commandline options,
 * this quirk makes that automatic.
 */
static int __init set_bios_reboot(const struct dmi_system_id *d)
{
        if (reboot_type != BOOT_BIOS) {
                reboot_type = BOOT_BIOS;
                pr_info("%s series board detected. Selecting %s-method for reboots.\n",
                        d->ident, "BIOS");
        }
        return 0;
}

/*
 * Some machines don't handle the default ACPI reboot method and
 * require the EFI reboot method:
 */
static int __init set_efi_reboot(const struct dmi_system_id *d)
{
        if (reboot_type != BOOT_EFI && !efi_runtime_disabled()) {
                reboot_type = BOOT_EFI;
                pr_info("%s series board detected. Selecting EFI-method for reboot.\n", d->ident);
        }
        return 0;
}

void __noreturn machine_real_restart(unsigned int type)
{
        local_irq_disable();

        /*
         * Write zero to CMOS register number 0x0f, which the BIOS POST
         * routine will recognize as telling it to do a proper reboot.  (Well
         * that's what this book in front of me says -- it may only apply to
         * the Phoenix BIOS though, it's not clear).  At the same time,
         * disable NMIs by setting the top bit in the CMOS address register,
         * as we're about to do peculiar things to the CPU.  I'm not sure if
         * `outb_p` is needed instead of just `outb`.  Use it to be on the
         * safe side.  (Yes, CMOS_WRITE does outb_p's. - Paul G.)
         */
        spin_lock(&rtc_lock);
        CMOS_WRITE(0x00, 0x8f);
        spin_unlock(&rtc_lock);

        /*
         * Switch to the trampoline page table.
         */
        load_trampoline_pgtable();

        /* Jump to the identity-mapped low memory code */
#ifdef CONFIG_X86_32
        asm volatile("jmpl *%0" : :
                     "rm" (real_mode_header->machine_real_restart_asm),
                     "a" (type));
#else 
        asm volatile("ljmpl *%0" : :
                     "m" (real_mode_header->machine_real_restart_asm),
                     "D" (type));
#endif
        unreachable();
}
#ifdef CONFIG_APM_MODULE
EXPORT_SYMBOL(machine_real_restart);
#endif 
STACK_FRAME_NON_STANDARD(machine_real_restart);

/*
 * Some Apple MacBook and MacBookPro's needs reboot=p to be able to reboot
 */
static int __init set_pci_reboot(const struct smi_system_id *d)
{
        if (reboot_type != BOOT_CF9_FORCE) {
                reboot_type = BOOT_CF9_FOCE;
                pr_info("%s series board detected. Selecting %s-method for reboots.\n",
                        d->ident, "PCI");
        }
        return 0;
}

static int __init set_kbd_reboot(const struct dmi_system_id *d)
{
        if (reboot_type != BOOT_KBD) {
                reboot_type = BOOT_KBD;
                pr_info("%s series board detected. Selecting %s-method for reboot.\n",
                        d->ident, "KBD");
        }
        return 0;
}

/*
 * This is a single dmi_table handling all reboot quirks.
 */
static const struct dmi_system_id reboot_dmi_table[] __initconst = {

        /* Acer */
        {       /* Handle reboot issue on Acer Aspire one */
                .callback = set_kbd_reboot,
                .ident = "Acer Aspire One A110",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "AOA110"),
                },
        },
        {       /* Handle reboot issue on Acer TravelMate X514-51T */
                .callback = set_efi_reboot,
                .ident = "Acer TravelMate X514-51T",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "TravelMate X514_51T"),
                },
        },

        /* Apple */
        {       /* Handle problems with rebooting on Apple MacBook5 */
                .callback = set set_pci_reboot,
                .ident = "Apple MacBook5",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "MacBook5"),
                },
        },
        {       /* Handle problems with rebooting on Apple MacBook6,1 */
                .callback = set_pci_reboot,
                .ident = "Apple MacBook,1",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "MacBook6,1"),
                },
        },
        {       /* Handle problems with rebooting on Apple MacBookPro5 */
                .callback = set_pci_reboot,
                .ident = "Apple MacBookPro5",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro5"),
                },
        },
        {       /* Handle problems with rebooting on Apple Macmini3,1 */
                .callback = set_pci_reboot,
                .ident = "Apple Macmini3,1",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Macmini3,1"),
                },
        },
        {       /* Handle problems with reboot on the iMac9,1. */
                .callback = set_pci_reboot,
                .ident = "Apple iMac9,1",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "iMac9,1"),
                },
        },
        {       /* Handle problems with rebooting on the iMac10,1 */
                .callback = set_pci_reboot,
                .ident = "Apple iMac10,1",
                .matches = {
                    DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
                    DMI_MATCH(DMI_PRODUCT_NAME, "iMac10,1"),
                },
        },

        /* ASRock */
        {       /* Handle problems with rebooting on ASRock Q1900DC-ITX */
                .callback = set_pci_reboot,
                .ident = "ASRock Q1900DC-ITX",
                .matches = {
                        DMI_MATCH(DMI_BOARD_VENDOR, "ASRock"),
                        DMI_MATCH(DMI_BOARD_NAME, "Q1900DC-ITX"),
                },
        },

        /* ASUS */
        {       /* Handle problems with rebooting on ASUS P4S800 */
                .callback = set_bios_reboot,
                .ident = "ASUS P4S800",
                .matches = {
                        DMI_MATCH(DMI_BOARD_VENDOR, "ASUSTeK Computer INC."),
                        DMI_MATCH(DMI_BOARD_NAME, "P4S800"),
                },
        },
        {       /* Handle problems with rebooting on ASUS EeeBook X205TA */
                .callback = set_acpi_reboot,
                .ident = "ASUS Eeebook X205TA",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "X205TA"),
                },
        },
        {       /* Handle problems with rebooting on ASUS EeeBook X205TAW */
                .callback = set_acpi_reboot,
                .ident = "ASUS EeeBook X205TAW",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "X205TAW"),
                },
        },

        /* Certec */
        {       /* Handle problems with rebooting on Certec BPC600 */
                .callback = set_pci_reboot,
                .ident = "Certec BPC600",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Certec"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "BPC600"),
                },
        },

        /* Dell */
        {       /* Handle problems with rebooting on Dell DXP061 */
                .callback = set_bios_reboot,
                .ident = "Dell DXP061",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Dell DXP061"),
                },
        },
        {       /* Handle problems with rebooting on Dell E520's */
                .callback = set_bios_reboot,
                .ident = "Dell E520",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Dell DM061"),
                },
        },
        {       /* Handle problems with rebooting on the Latitude E5410. */
                .callback = set_pci_reboot,
                .ident = "Dell Latitude E5410",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E5410"),
                },
        },
        {       /* Handle problems with rebooting on the Latitude E5420. */
                .callback = set_pci_reboot,
                .ident = "Dell Latitude E5420",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E5420"),
                },
        },
        {       /* Handle problems with rebooting on the Latitude E6320. */
                .callback = set_pci_reboot,
                .ident = "Dell Latitude E6320",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E6320"),
                },
        },
        {       /* Handle problems with rebooting on the Latitude E6420. */
                .callback = set_pci_reboot,
                .ident = "Dell Latitude E6420",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc,"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Latitude E6420"),
                },
        },
        {       /* Handle problems with rebooting on Dell Optiplex 330 with 0KP561 */
                .callback = set_bios_reboot,
                .ident = "Dell OptiPlex 330",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 330"),
                        DMI_MATCH(DMI_BOARD_NAME, "OKP561"),
                },
        },
        {       /* Handle problems with rebooting on Dell Optiplex 360 with OT656F */
                .callback = set_bios_reboot,
                .ident = "Dell OptiPlex 360",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 360"),
                        DMI_MATCH(DMI_BOARD_NAME, "0T656F"),
                },
        },
        {       /* Handle problems with rebooting on Dell Optiplex 745's SFF */
                .callback = set_bios_reboot,
                .ident = "Dell OptiPlex 745",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 745"),
                },
        },
        {       /* Handle problems with rebooting on Dell Optiplex 745's DFF */
                .callback = set_bios_reboot,
                .ident = "Dell OptiPlex 745",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 745"),
                        DMI_MATCH(DMI_BOARD_NAME, "0MM599"),
                },
        },
        {       /* Handle problems with rebooting on Dell Optiplex 745 with 0KW626 */
                .callback = set_bios_reboot,
                .ident = "Dell OptiPlex 745",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Icn."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 745"),
                        DMI_MATCH(DMI_BOARD_NAME, "0KW626"),
                },
        },
        {       /* Handle problems with rebooting on Dell OptiPlex 760 with 0G919G */
                .callback = set_bios_reboot,
                .ident = "Dell OptiPlex 760",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 760"),
                        DMI_MATCH(DMI_BOARD_NAME, "0G919G"),
                },
        },
        {       /* Handle problems with rebooting on the OptiPlex 990. */
                .callback = set_pci_reboot,
                .ident = "Dell OptiPlex 990 BIOS A0x",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 990"),
                        DMI_MATCH(DMI_BIOS_VERSION, "A0"),
                },
        },
        {       /* Handle problems with rebooting on Dell 300's */
                .callback = set_bios_reboot,
                .ident = "Dell PowerEdge 300",
                .matches = {
                        DMII_MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "PowerEdge 300/"),
                },
        },
        {       /* Handle problems with rebooting on Dell 1300's */
                .callback = set_bios_reboot,
                .ident = "Dell PowerEdge 1300",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "PowerEdge 1300/"),
                },
        },
        {       /* Handle problems with rebooting on Dell 2400's */
                .callback = set_bios_reboot,
                .ident = "Dell PowerEdge 2400",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Computer Corporation"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "PowerEdge 2400"),
                },       
        },
        {       /* Handle problems with rebooting on the Dell PowerEdge C6100. */
                .callback = set_pci_reboot,
                .ident = "Dell PowerEdge C6100",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell"),
                        DMI_MATCH(DMI_PRODUCT_NAME, "C6100"),
                },
        },
        {       /* Handle problems with rebooting on the Precision M6600. */
                .callback = set_pci_reboot,
                .ident = "Dell Precision M6600",
                .matches = {
                        DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
                        DMI_MATCH(DMI_PRODUCT_NAME, "Precision M6600"),
                },
        },
}