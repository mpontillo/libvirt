#include <config.h>
#ifdef WITH_QEMU

# include "testutilsqemu.h"
# include "testutilshostcpus.h"
# include "testutils.h"
# include "viralloc.h"
# include "cpu_conf.h"
# include "qemu/qemu_domain.h"
# define LIBVIRT_QEMU_CAPSPRIV_H_ALLOW
# include "qemu/qemu_capspriv.h"
# include "virstring.h"
# include "virfilecache.h"
# include "virtpm.h"

# include <sys/types.h>
# include <fcntl.h>

# define VIR_FROM_THIS VIR_FROM_QEMU

static virCPUDef *cpuDefault;
static virCPUDef *cpuHaswell;
static virCPUDef *cpuPower8;
static virCPUDef *cpuPower9;
static virCPUDef *cpuPower10;


static const char *qemu_emulators[VIR_ARCH_LAST] = {
    [VIR_ARCH_I686] = "/usr/bin/qemu-system-i386",
    [VIR_ARCH_X86_64] = "/usr/bin/qemu-system-x86_64",
    [VIR_ARCH_AARCH64] = "/usr/bin/qemu-system-aarch64",
    [VIR_ARCH_PPC64] = "/usr/bin/qemu-system-ppc64",
    [VIR_ARCH_S390X] = "/usr/bin/qemu-system-s390x",
};

static const virArch arch_alias[VIR_ARCH_LAST] = {
    [VIR_ARCH_PPC64LE] = VIR_ARCH_PPC64,
};

static const char *const i386_machines[] = {
    "pc", NULL
};

static const char *const x86_64_machines[] = {
    "pc", "q35", NULL
};
static const char *const aarch64_machines[] = {
    "virt", "virt-2.6", "versatilepb", NULL
};
static const char *const ppc64_machines[] = {
    "pseries", NULL
};
static const char *const s390x_machines[] = {
    "s390-ccw-virtio", NULL
};

static const char *const *qemu_machines[VIR_ARCH_LAST] = {
    [VIR_ARCH_I686] = i386_machines,
    [VIR_ARCH_X86_64] = x86_64_machines,
    [VIR_ARCH_AARCH64] = aarch64_machines,
    [VIR_ARCH_PPC64] = ppc64_machines,
    [VIR_ARCH_S390X] = s390x_machines,
};

static const char *const *hvf_machines[VIR_ARCH_LAST] = {
    [VIR_ARCH_I686] = NULL,
    [VIR_ARCH_X86_64] = x86_64_machines,
    [VIR_ARCH_AARCH64] = aarch64_machines,
    [VIR_ARCH_PPC64] = NULL,
    [VIR_ARCH_RISCV64] = NULL,
    [VIR_ARCH_S390X] = NULL,
};

static const char *qemu_default_ram_id[VIR_ARCH_LAST] = {
    [VIR_ARCH_I686] = "pc.ram",
    [VIR_ARCH_X86_64] = "pc.ram",
    [VIR_ARCH_AARCH64] = "mach-virt.ram",
    [VIR_ARCH_PPC64] = "ppc_spapr.ram",
    [VIR_ARCH_S390X] = "s390.ram",
};

char *
virFindFileInPath(const char *file)
{
    if (g_str_has_prefix(file, "qemu-system") ||
        g_str_equal(file, "qemu-kvm")) {
        return g_strdup_printf("/usr/bin/%s", file);
    }

    /* Nothing in tests should be relying on real files
     * in host OS, so we return NULL to try to force
     * an error in such a case
     */
    return NULL;
}


/* Enough to tell capabilities code that swtpm is usable */
bool virTPMHasSwtpm(void)
{
    return true;
}



bool
virTPMSwtpmSetupCapsGet(virTPMSwtpmSetupFeature cap)
{
    const char *tpmver = getenv(TEST_TPM_ENV_VAR);

    switch (cap) {
    case VIR_TPM_SWTPM_SETUP_FEATURE_TPM_1_2:
        if (!tpmver || (tpmver && strstr(tpmver, TPM_VER_1_2)))
            return true;
        break;
    case VIR_TPM_SWTPM_SETUP_FEATURE_TPM_2_0:
        if (!tpmver || (tpmver && strstr(tpmver, TPM_VER_2_0)))
            return true;
        break;
    case VIR_TPM_SWTPM_SETUP_FEATURE_CMDARG_PWDFILE_FD:
    case VIR_TPM_SWTPM_SETUP_FEATURE_CMDARG_CREATE_CONFIG_FILES:
    case VIR_TPM_SWTPM_SETUP_FEATURE_TPM12_NOT_NEED_ROOT:
    case VIR_TPM_SWTPM_SETUP_FEATURE_CMDARG_RECONFIGURE_PCR_BANKS:
    case VIR_TPM_SWTPM_SETUP_FEATURE_LAST:
        break;
    }

    return false;
}


virCapsHostNUMA *
virCapabilitiesHostNUMANewHost(void)
{
    /*
     * Build a NUMA topology with cell_id (NUMA node id
     * being 3(0 + 3),4(1 + 3), 5 and 6
     */
    return virTestCapsBuildNUMATopology(3);
}

void
virHostCPUX86GetCPUID(uint32_t leaf,
                      uint32_t extended,
                      uint32_t *eax,
                      uint32_t *ebx,
                      uint32_t *ecx,
                      uint32_t *edx)
{
    if (eax)
        *eax = 0;
    if (ebx)
        *ebx = 0;
    if (ecx)
        *ecx = 0;
    if (edx)
        *edx = 0;
    if (leaf == 0x8000001F && extended == 0) {
        if (ecx)
            *ecx = 509;
        if (edx)
            *edx = 451;
    }
}

static int
testQemuAddGuest(virCaps *caps,
                 virArch arch,
                 testQemuHostOS hostOS)
{
    size_t nmachines;
    virCapsGuestMachine **machines = NULL;
    virCapsGuest *guest;
    virArch emu_arch = arch;

    if (arch_alias[arch] != VIR_ARCH_NONE)
        emu_arch = arch_alias[arch];

    if (qemu_emulators[emu_arch] == NULL)
        return 0;

    nmachines = g_strv_length((gchar **)qemu_machines[emu_arch]);
    machines = virCapabilitiesAllocMachines(qemu_machines[emu_arch],
                                            nmachines);
    if (machines == NULL)
        goto error;

    guest = virCapabilitiesAddGuest(caps, VIR_DOMAIN_OSTYPE_HVM,
                                    arch, qemu_emulators[emu_arch],
                                    NULL, nmachines, machines);

    machines = NULL;
    nmachines = 0;

    if (arch == VIR_ARCH_I686 ||
        arch == VIR_ARCH_X86_64)
        virCapabilitiesAddGuestFeature(guest, VIR_CAPS_GUEST_FEATURE_TYPE_CPUSELECTION);

    virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_QEMU,
                                  NULL, NULL, 0, NULL);

    if (hostOS == HOST_OS_LINUX) {
        nmachines = g_strv_length((char **)qemu_machines[emu_arch]);
        machines = virCapabilitiesAllocMachines(qemu_machines[emu_arch],
                                                nmachines);
        if (machines == NULL)
            goto error;

        virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_KVM,
                                      qemu_emulators[emu_arch],
                                      NULL, nmachines, machines);
    }

    if (hostOS == HOST_OS_MACOS) {
        if (hvf_machines[emu_arch] != NULL) {
            nmachines = g_strv_length((char **)hvf_machines[emu_arch]);
            machines = virCapabilitiesAllocMachines(hvf_machines[emu_arch],
                                                    nmachines);
            if (machines == NULL)
                goto error;

            virCapabilitiesAddGuestDomain(guest, VIR_DOMAIN_VIRT_HVF,
                                          qemu_emulators[emu_arch],
                                          NULL, nmachines, machines);
        }
    }

    return 0;

 error:
    virCapabilitiesFreeMachines(machines, nmachines);
    return -1;
}


static virCaps*
testQemuCapsInitImpl(testQemuHostOS hostOS)
{
    virCaps *caps;
    size_t i;

    if (!(caps = virCapabilitiesNew(VIR_ARCH_X86_64, false, false)))
        return NULL;

    /* Add dummy 'none' security_driver. This is equal to setting
     * security_driver = "none" in qemu.conf. */
    caps->host.secModels = g_new0(virCapsHostSecModel, 1);
    caps->host.nsecModels = 1;

    caps->host.secModels[0].model = g_strdup("none");
    caps->host.secModels[0].doi = g_strdup("0");

    if (!(caps->host.numa = virCapabilitiesHostNUMANewHost()))
        goto cleanup;

    for (i = 0; i < VIR_ARCH_LAST; i++) {
        if (testQemuAddGuest(caps, i, hostOS) < 0)
            goto cleanup;
    }

    if (virTestGetDebug()) {
        g_autofree char *caps_str = NULL;

        caps_str = virCapabilitiesFormatXML(caps);
        if (!caps_str)
            goto cleanup;

        VIR_TEST_DEBUG("QEMU driver capabilities:\n%s", caps_str);
    }

    return caps;

 cleanup:
    caps->host.cpu = NULL;
    virObjectUnref(caps);
    return NULL;
}

virCaps*
testQemuCapsInit(void)
{
    return testQemuCapsInitImpl(HOST_OS_LINUX);
}

virCaps*
testQemuCapsInitMacOS(void)
{
    return testQemuCapsInitImpl(HOST_OS_MACOS);
}


virCPUDef *
qemuTestGetCPUDef(qemuTestCPUDef d)
{
    switch (d) {
    case QEMU_CPU_DEF_DEFAULT: return cpuDefault;
    case QEMU_CPU_DEF_HASWELL: return cpuHaswell;
    case QEMU_CPU_DEF_POWER8: return cpuPower8;
    case QEMU_CPU_DEF_POWER9: return cpuPower9;
    case QEMU_CPU_DEF_POWER10: return cpuPower10;
    }

    return NULL;
}


void
qemuTestSetHostArch(virQEMUDriver *driver,
                    virArch arch)
{
    if (arch == VIR_ARCH_NONE)
        arch = VIR_ARCH_X86_64;

    virTestSetHostArch(arch);
    driver->hostarch = virArchFromHost();
    driver->caps->host.arch = virArchFromHost();
    qemuTestSetHostCPU(driver, arch, NULL);
}


void
qemuTestSetHostCPU(virQEMUDriver *driver,
                   virArch arch,
                   virCPUDef *cpu)
{
    if (!cpu) {
        if (ARCH_IS_X86(arch))
            cpu = cpuDefault;
        else if (ARCH_IS_PPC64(arch))
            cpu = cpuPower8;
    }

    g_unsetenv("VIR_TEST_MOCK_FAKE_HOST_CPU");
    if (cpu) {
        if (cpu->model)
            g_setenv("VIR_TEST_MOCK_FAKE_HOST_CPU", cpu->model, TRUE);
    }
    if (driver) {
        if (cpu)
            driver->caps->host.arch = cpu->arch;
        driver->caps->host.cpu = cpu;

        virCPUDefFree(driver->hostcpu);
        if (cpu)
            virCPUDefRef(cpu);
        driver->hostcpu = cpu;
    }
}


virQEMUCaps *
qemuTestParseCapabilitiesArch(virArch arch,
                              const char *capsFile)
{
    g_autofree char *binary = g_strdup_printf("/usr/bin/qemu-system-%s",
                                              virArchToString(arch));
    g_autoptr(virQEMUCaps) qemuCaps = virQEMUCapsNewBinary(binary);

    if (virQEMUCapsLoadCache(arch, qemuCaps, capsFile, true) < 0)
        return NULL;

    return g_steal_pointer(&qemuCaps);
}


void qemuTestDriverFree(virQEMUDriver *driver)
{
    virMutexDestroy(&driver->lock);
    if (driver->config) {
        virFileDeleteTree(driver->config->stateDir);
        virFileDeleteTree(driver->config->configDir);
    }
    virObjectUnref(driver->qemuCapsCache);
    virObjectUnref(driver->xmlopt);
    virObjectUnref(driver->caps);
    virObjectUnref(driver->config);
    virObjectUnref(driver->securityManager);

    virCPUDefFree(cpuDefault);
    virCPUDefFree(cpuHaswell);
    virCPUDefFree(cpuPower8);
    virCPUDefFree(cpuPower9);
    virCPUDefFree(cpuPower10);
}


static void
qemuTestCapsPopulateFakeMachines(virQEMUCaps *caps,
                                 virArch arch,
                                 testQemuHostOS hostOS)
{
    size_t i;
    const char *defaultRAMid = NULL;

    /* default-ram-id appeared in QEMU 5.2.0. Reflect
     * this in our capabilities, i.e. set it for new
     * enough versions only. */
    if (virQEMUCapsGetVersion(caps) >= 5002000)
        defaultRAMid = qemu_default_ram_id[arch];

    virQEMUCapsSetArch(caps, arch);

    for (i = 0; qemu_machines[arch][i] != NULL; i++) {
        virQEMUCapsAddMachine(caps,
                              VIR_DOMAIN_VIRT_QEMU,
                              qemu_machines[arch][i],
                              NULL,
                              NULL,
                              0,
                              false,
                              false,
                              true,
                              defaultRAMid,
                              false,
                              VIR_TRISTATE_BOOL_ABSENT);
        virQEMUCapsSet(caps, QEMU_CAPS_TCG);

        if (hostOS == HOST_OS_LINUX) {
            virQEMUCapsAddMachine(caps,
                                  VIR_DOMAIN_VIRT_KVM,
                                  qemu_machines[arch][i],
                                  NULL,
                                  NULL,
                                  0,
                                  false,
                                  false,
                                  true,
                                  defaultRAMid,
                                  false,
                                  VIR_TRISTATE_BOOL_ABSENT);
            virQEMUCapsSet(caps, QEMU_CAPS_KVM);
        }
    }

    if (hostOS == HOST_OS_MACOS) {
        if (hvf_machines[arch] != NULL) {
            for (i = 0; hvf_machines[arch][i] != NULL; i++) {
                virQEMUCapsAddMachine(caps,
                        VIR_DOMAIN_VIRT_HVF,
                        hvf_machines[arch][i],
                        NULL,
                        NULL,
                        0,
                        false,
                        false,
                        true,
                        defaultRAMid,
                        false,
                        VIR_TRISTATE_BOOL_ABSENT);
                virQEMUCapsSet(caps, QEMU_CAPS_HVF);
            }
        }
    }
}


static int
qemuTestCapsCacheInsertData(virFileCache *cache,
                            const char *binary,
                            virQEMUCaps *caps)
{
    if (virFileCacheInsertData(cache, binary, virObjectRef(caps)) < 0) {
        virObjectUnref(caps);
        return -1;
    }

    return 0;
}


static int
qemuTestCapsCacheInsertImpl(virFileCache *cache,
                            virQEMUCaps *caps,
                            testQemuHostOS hostOS)
{
    size_t i;

    if (virQEMUCapsGetArch(caps) != VIR_ARCH_NONE) {
        /* all tests using real caps or arcitecture are expected to call:
         *
         *  virFileCacheClear(driver.qemuCapsCache);
         *
         * before populating the cache;
         */
        /* caps->binary is populated only for real capabilities */
        if (virQEMUCapsGetBinary(caps)) {
            if (qemuTestCapsCacheInsertData(cache, virQEMUCapsGetBinary(caps), caps) < 0)
                return -1;
        } else {
            virArch arch = virQEMUCapsGetArch(caps);
            g_autoptr(virQEMUCaps) copyCaps = NULL;
            virQEMUCaps *effCaps = caps;

            if (arch_alias[arch] != VIR_ARCH_NONE)
                arch = arch_alias[arch];

            if (qemu_emulators[arch]) {
                /* if we are dealing with fake caps we need to populate machine types */
                if (!virQEMUCapsHasMachines(caps)) {
                    copyCaps = effCaps = virQEMUCapsNewCopy(caps);
                    qemuTestCapsPopulateFakeMachines(copyCaps, arch, hostOS);
                }

                if (qemuTestCapsCacheInsertData(cache, qemu_emulators[arch], effCaps) < 0)
                    return -1;
            }
        }
    } else {
        /* in case when caps are missing or are missing architecture, we populate
         * everything */
        for (i = 0; i < G_N_ELEMENTS(qemu_emulators); i++) {
            g_autoptr(virQEMUCaps) tmp = NULL;

            if (qemu_emulators[i] == NULL)
                continue;

            tmp = virQEMUCapsNewCopy(caps);

            qemuTestCapsPopulateFakeMachines(tmp, i, hostOS);

            if (qemuTestCapsCacheInsertData(cache, qemu_emulators[i], tmp) < 0)
                return -1;
        }
    }

    return 0;
}

int
qemuTestCapsCacheInsert(virFileCache *cache,
                        virQEMUCaps *caps)
{
    return qemuTestCapsCacheInsertImpl(cache, caps, HOST_OS_LINUX);
}

int
qemuTestCapsCacheInsertMacOS(virFileCache *cache,
                             virQEMUCaps *caps)
{
    return qemuTestCapsCacheInsertImpl(cache, caps, HOST_OS_MACOS);
}


# define STATEDIRTEMPLATE abs_builddir "/qemustatedir-XXXXXX"
# define CONFIGDIRTEMPLATE abs_builddir "/qemuconfigdir-XXXXXX"

int qemuTestDriverInit(virQEMUDriver *driver)
{
    virQEMUDriverConfig *cfg = NULL;
    virSecurityManager *mgr = NULL;
    char statedir[] = STATEDIRTEMPLATE;
    char configdir[] = CONFIGDIRTEMPLATE;
    g_autoptr(virQEMUCaps) emptyCaps = NULL;

    memset(driver, 0, sizeof(*driver));

    cpuDefault = virCPUDefCopy(&cpuDefaultData);
    cpuHaswell = virCPUDefCopy(&cpuHaswellData);
    cpuPower8 = virCPUDefCopy(&cpuPower8Data);
    cpuPower9 = virCPUDefCopy(&cpuPower9Data);
    cpuPower10 = virCPUDefCopy(&cpuPower10Data);

    if (virMutexInit(&driver->lock) < 0)
        return -1;

    driver->hostarch = virArchFromHost();

    cfg = virQEMUDriverConfigNew(false, NULL);
    if (!cfg)
        goto error;
    driver->config = cfg;

    /* Do this early so that qemuTestDriverFree() doesn't see (unlink) the real
     * dirs. */
    VIR_FREE(cfg->stateDir);
    VIR_FREE(cfg->configDir);

    /* Override paths to ensure predictable output
     *
     * FIXME Find a way to achieve the same result while avoiding
     *       code duplication
     */
    VIR_FREE(cfg->libDir);
    cfg->libDir = g_strdup("/var/lib/libvirt/qemu");
    VIR_FREE(cfg->channelTargetDir);
    cfg->channelTargetDir = g_strdup("/var/lib/libvirt/qemu/channel/target");
    VIR_FREE(cfg->memoryBackingDir);
    cfg->memoryBackingDir = g_strdup("/var/lib/libvirt/qemu/ram");
    VIR_FREE(cfg->nvramDir);
    cfg->nvramDir = g_strdup("/var/lib/libvirt/qemu/nvram");
    VIR_FREE(cfg->passtStateDir);
    cfg->passtStateDir = g_strdup("/var/run/libvirt/qemu/passt");
    VIR_FREE(cfg->dbusStateDir);
    cfg->dbusStateDir = g_strdup("/var/run/libvirt/qemu/dbus");

    if (!g_mkdtemp(statedir)) {
        fprintf(stderr, "Cannot create fake stateDir");
        goto error;
    }

    cfg->stateDir = g_strdup(statedir);

    if (!g_mkdtemp(configdir)) {
        fprintf(stderr, "Cannot create fake configDir");
        goto error;
    }

    cfg->configDir = g_strdup(configdir);

    driver->caps = testQemuCapsInit();
    if (!driver->caps)
        goto error;

    /* Using /dev/null for libDir and cacheDir automatically produces errors
     * upon attempt to use any of them */
    driver->qemuCapsCache = virQEMUCapsCacheNew("/dev/null", "/dev/null", 0, 0);
    if (!driver->qemuCapsCache)
        goto error;

    driver->xmlopt = virQEMUDriverCreateXMLConf(driver, "none");
    if (!driver->xmlopt)
        goto error;

    /* Populate the capabilities cache with fake empty caps */
    emptyCaps = virQEMUCapsNew();
    if (qemuTestCapsCacheInsert(driver->qemuCapsCache, emptyCaps) < 0)
        goto error;

    if (!(mgr = virSecurityManagerNew("none", "qemu",
                                      VIR_SECURITY_MANAGER_PRIVILEGED)))
        goto error;
    if (!(driver->securityManager = virSecurityManagerNewStack(mgr)))
        goto error;

    qemuTestSetHostCPU(driver, driver->hostarch, NULL);

    VIR_FREE(cfg->vncTLSx509certdir);
    cfg->vncTLSx509certdir = g_strdup("/etc/pki/libvirt-vnc");
    VIR_FREE(cfg->spiceTLSx509certdir);
    cfg->spiceTLSx509certdir = g_strdup("/etc/pki/libvirt-spice");
    VIR_FREE(cfg->chardevTLSx509certdir);
    cfg->chardevTLSx509certdir = g_strdup("/etc/pki/libvirt-chardev");
    VIR_FREE(cfg->vxhsTLSx509certdir);
    cfg->vxhsTLSx509certdir = g_strdup("/etc/pki/libvirt-vxhs");
    VIR_FREE(cfg->nbdTLSx509certdir);
    cfg->nbdTLSx509certdir = g_strdup("/etc/pki/libvirt-nbd");
    VIR_FREE(cfg->migrateTLSx509certdir);
    cfg->migrateTLSx509certdir = g_strdup("/etc/pki/libvirt-migrate");
    VIR_FREE(cfg->backupTLSx509certdir);
    cfg->backupTLSx509certdir = g_strdup("/etc/pki/libvirt-backup");

    VIR_FREE(cfg->vncSASLdir);
    cfg->vncSASLdir = g_strdup("/etc/sasl2");
    VIR_FREE(cfg->spiceSASLdir);
    cfg->spiceSASLdir = g_strdup("/etc/sasl2");

    VIR_FREE(cfg->spicePassword);
    cfg->spicePassword = g_strdup("123456");

    VIR_FREE(cfg->hugetlbfs);
    cfg->hugetlbfs = g_new0(virHugeTLBFS, 2);
    cfg->nhugetlbfs = 2;
    cfg->hugetlbfs[0].mnt_dir = g_strdup("/dev/hugepages2M");
    cfg->hugetlbfs[1].mnt_dir = g_strdup("/dev/hugepages1G");
    cfg->hugetlbfs[0].size = 2048;
    cfg->hugetlbfs[0].deflt = true;
    cfg->hugetlbfs[1].size = 1048576;

    driver->privileged = true;

    return 0;

 error:
    virObjectUnref(mgr);
    qemuTestDriverFree(driver);
    return -1;
}

int
testQemuCapsSetGIC(virQEMUCaps *qemuCaps,
                   int gic)
{
    virGICCapability *gicCapabilities = NULL;
    size_t ngicCapabilities = 0;

    gicCapabilities = g_new0(virGICCapability, 2);

# define IMPL_BOTH \
         VIR_GIC_IMPLEMENTATION_KERNEL|VIR_GIC_IMPLEMENTATION_EMULATED

    if (gic & GIC_V2) {
        gicCapabilities[ngicCapabilities].version = VIR_GIC_VERSION_2;
        gicCapabilities[ngicCapabilities].implementation = IMPL_BOTH;
        ngicCapabilities++;
    }
    if (gic & GIC_V3) {
        gicCapabilities[ngicCapabilities].version = VIR_GIC_VERSION_3;
        gicCapabilities[ngicCapabilities].implementation = IMPL_BOTH;
        ngicCapabilities++;
    }

# undef IMPL_BOTH

    virQEMUCapsSetGICCapabilities(qemuCaps,
                                  gicCapabilities, ngicCapabilities);

    return 0;
}

#endif


char *
testQemuGetLatestCapsForArch(const char *arch,
                             const char *suffix)
{
    struct dirent *ent;
    g_autoptr(DIR) dir = NULL;
    int rc;
    g_autofree char *fullsuffix = NULL;
    unsigned long maxver = 0;
    unsigned long ver;
    g_autofree char *maxname = NULL;

    fullsuffix = g_strdup_printf("%s.%s", arch, suffix);

    if (virDirOpen(&dir, TEST_QEMU_CAPS_PATH) < 0)
        return NULL;

    while ((rc = virDirRead(dir, &ent, TEST_QEMU_CAPS_PATH)) > 0) {
        g_autofree char *tmp = NULL;

        tmp = g_strdup(STRSKIP(ent->d_name, "caps_"));

        if (!tmp)
            continue;

        if (!virStringStripSuffix(tmp, fullsuffix))
            continue;

        if (virStringParseVersion(&ver, tmp, false) < 0) {
            VIR_TEST_DEBUG("skipping caps file '%s'", ent->d_name);
            continue;
        }

        if (ver > maxver) {
            g_free(maxname);
            maxname = g_strdup(ent->d_name);
            maxver = ver;
        }
    }

    if (rc < 0)
        return NULL;

    if (!maxname) {
        VIR_TEST_VERBOSE("failed to find capabilities for '%s' in '%s'",
                         arch, TEST_QEMU_CAPS_PATH);
        return NULL;
    }

    return g_strdup_printf("%s/%s", TEST_QEMU_CAPS_PATH, maxname);
}


GHashTable *
testQemuGetLatestCaps(void)
{
    const char *archs[] = {
        "aarch64",
        "ppc64",
        "riscv64",
        "s390x",
        "x86_64",
        "sparc",
        "ppc",
    };
    g_autoptr(GHashTable) capslatest = virHashNew(g_free);
    size_t i;

    VIR_TEST_VERBOSE("");

    for (i = 0; i < G_N_ELEMENTS(archs); ++i) {
        char *cap = testQemuGetLatestCapsForArch(archs[i], "xml");

        if (!cap || virHashAddEntry(capslatest, archs[i], cap) < 0)
            return NULL;

        VIR_TEST_VERBOSE("latest caps for %s: %s", archs[i], cap);
    }

    VIR_TEST_VERBOSE("");

    return g_steal_pointer(&capslatest);
}


int
testQemuCapsIterate(const char *suffix,
                    testQemuCapsIterateCallback callback,
                    void *opaque)
{
    struct dirent *ent;
    g_autoptr(DIR) dir = NULL;
    int rc;
    bool fail = false;

    if (!callback)
        return 0;

    /* Validate suffix */
    if (!STRPREFIX(suffix, ".")) {
        VIR_TEST_VERBOSE("malformed suffix '%s'", suffix);
        return -1;
    }

    if (virDirOpen(&dir, TEST_QEMU_CAPS_PATH) < 0)
        return -1;

    while ((rc = virDirRead(dir, &ent, TEST_QEMU_CAPS_PATH)) > 0) {
        g_autofree char *tmp = g_strdup(ent->d_name);
        char *version = NULL;
        char *archName = NULL;

        /* Strip the trailing suffix, moving on if it's not present */
        if (!virStringStripSuffix(tmp, suffix))
            continue;

        /* Strip the leading prefix */
        if (!(version = STRSKIP(tmp, "caps_"))) {
            VIR_TEST_VERBOSE("malformed file name '%s'", ent->d_name);
            return -1;
        }

        /* Find the last dot */
        if (!(archName = strrchr(tmp, '.'))) {
            VIR_TEST_VERBOSE("malformed file name '%s'", ent->d_name);
            return -1;
        }

        /* The version number and the architecture name are separated by
         * a dot: overwriting that dot with \0 results in both being usable
         * as independent, null-terminated strings */
        archName[0] = '\0';
        archName++;

        /* Run the user-provided callback.
         *
         * We skip the dot that, as verified earlier, starts the suffix
         * to make it nicer to rebuild the original file name from inside
         * the callback.
         */
        if (callback(TEST_QEMU_CAPS_PATH, "caps", version,
                     archName, suffix + 1, opaque) < 0)
            fail = true;
    }

    if (rc < 0 || fail)
        return -1;

    return 0;
}


void
testQemuInfoSetArgs(struct testQemuInfo *info,
                    struct testQemuConf *conf, ...)
{
    va_list argptr;
    testQemuInfoArgName argname;
    int flag;

    info->conf = conf;
    info->args.newargs = true;

    va_start(argptr, conf);
    while ((argname = va_arg(argptr, testQemuInfoArgName)) != ARG_END) {
        switch (argname) {
        case ARG_QEMU_CAPS:
            if (!(info->args.fakeCapsAdd))
                info->args.fakeCapsAdd = virBitmapNew(QEMU_CAPS_LAST);

            while ((flag = va_arg(argptr, int)) < QEMU_CAPS_LAST)
                ignore_value(virBitmapSetBit(info->args.fakeCapsAdd, flag));
            break;

        case ARG_QEMU_CAPS_DEL:
            if (!(info->args.fakeCapsDel))
                info->args.fakeCapsDel = virBitmapNew(QEMU_CAPS_LAST);

            while ((flag = va_arg(argptr, int)) < QEMU_CAPS_LAST)
                ignore_value(virBitmapSetBit(info->args.fakeCapsDel, flag));
            break;

        case ARG_GIC:
            info->args.gic = va_arg(argptr, int);
            break;

        case ARG_MIGRATE_FROM:
            info->migrateFrom = va_arg(argptr, char *);
            break;

        case ARG_MIGRATE_FD:
            info->migrateFd = va_arg(argptr, int);
            break;

        case ARG_FLAGS:
            info->flags = va_arg(argptr, int);
            break;

        case ARG_PARSEFLAGS:
            info->parseFlags = va_arg(argptr, int);
            break;

        case ARG_CAPS_ARCH:
            info->args.capsarch = va_arg(argptr, char *);
            break;

        case ARG_CAPS_VER:
            info->args.capsver = va_arg(argptr, char *);
            break;

        case ARG_CAPS_HOST_CPU_MODEL:
            info->args.capsHostCPUModel = va_arg(argptr, int);
            break;

        case ARG_HOST_OS:
            info->args.hostOS = va_arg(argptr, int);
            break;

        case ARG_FD_GROUP: {
            virStorageSourceFDTuple *new = virStorageSourceFDTupleNew();
            const char *fdname = va_arg(argptr, char *);
            VIR_AUTOCLOSE fakefd = open("/dev/zero", O_RDWR);
            size_t i;

            new->nfds = va_arg(argptr, unsigned int);
            new->fds = g_new0(int, new->nfds);
            new->testfds = g_new0(int, new->nfds);

            for (i = 0; i < new->nfds; i++) {
                new->testfds[i] = va_arg(argptr, unsigned int);

                if (fcntl(new->testfds[i], F_GETFD) != -1) {
                    fprintf(stderr, "fd '%d' is already in use\n", new->fds[i]);
                    abort();
                }

                if ((new->fds[i] = dup(fakefd)) < 0) {
                    fprintf(stderr, "failed to duplicate fake fd: %s",
                            g_strerror(errno));
                    abort();
                }
            }

            if (!info->args.fds)
                info->args.fds = virHashNew(g_object_unref);

            g_hash_table_insert(info->args.fds, g_strdup(fdname), new);
            break;
        }

        case ARG_END:
        default:
            info->args.invalidarg = true;
            break;
        }

        if (info->args.invalidarg)
            break;
    }

    va_end(argptr);
}


int
testQemuInfoInitArgs(struct testQemuInfo *info)
{
    g_autofree char *capsfile = NULL;
    ssize_t cap;

    if (!info->args.newargs)
        return 0;

    info->args.newargs = false;

    if (info->args.invalidarg) {
        fprintf(stderr, "Invalid argument encountered by 'testQemuInfoSetArgs'\n");
        return -1;
    }

    if (!!info->args.capsarch ^ !!info->args.capsver) {
        fprintf(stderr, "ARG_CAPS_ARCH and ARG_CAPS_VER must be specified together.\n");
        return -1;
    }

    if (info->args.capsarch && info->args.capsver) {
        bool stripmachinealiases = false;
        virQEMUCaps *cachedcaps = NULL;

        info->arch = virArchFromString(info->args.capsarch);

        if (STREQ(info->args.capsver, "latest")) {
            capsfile = g_strdup(virHashLookup(info->conf->capslatest, info->args.capsarch));

            if (!capsfile) {
                fprintf(stderr, "'latest' caps for '%s' were not found\n", info->args.capsarch);
                return -1;
            }

            stripmachinealiases = true;
        } else {
            capsfile = g_strdup_printf("%s/caps_%s.%s.xml",
                                       TEST_QEMU_CAPS_PATH,
                                       info->args.capsver,
                                       info->args.capsarch);
        }

        if (!g_hash_table_lookup_extended(info->conf->capscache, capsfile, NULL, (void **) &cachedcaps)) {
            if (!(cachedcaps = qemuTestParseCapabilitiesArch(info->arch, capsfile)))
                return -1;

            g_hash_table_insert(info->conf->capscache, g_strdup(capsfile), cachedcaps);
        }

        info->qemuCaps = virQEMUCapsNewCopy(cachedcaps);

        if (stripmachinealiases)
            virQEMUCapsStripMachineAliases(info->qemuCaps);

        info->flags |= FLAG_REAL_CAPS;

        /* provide path to the replies file for schema testing */
        capsfile[strlen(capsfile) - 3] = '\0';
        info->schemafile = g_strdup_printf("%sreplies", capsfile);
    } else {
        info->qemuCaps = virQEMUCapsNew();
    }

    for (cap = -1; (cap = virBitmapNextSetBit(info->args.fakeCapsAdd, cap)) >= 0;)
        virQEMUCapsSet(info->qemuCaps, cap);

    for (cap = -1; (cap = virBitmapNextSetBit(info->args.fakeCapsDel, cap)) >= 0;)
        virQEMUCapsClear(info->qemuCaps, cap);

    if (info->args.gic != GIC_NONE &&
        testQemuCapsSetGIC(info->qemuCaps, info->args.gic) < 0)
        return -1;

    return 0;
}


void
testQemuInfoClear(struct testQemuInfo *info)
{
    VIR_FREE(info->infile);
    VIR_FREE(info->outfile);
    VIR_FREE(info->schemafile);
    VIR_FREE(info->errfile);
    virObjectUnref(info->qemuCaps);
    g_clear_pointer(&info->args.fakeCapsAdd, virBitmapFree);
    g_clear_pointer(&info->args.fakeCapsDel, virBitmapFree);
    g_clear_pointer(&info->args.fds, g_hash_table_unref);
}


/**
 * testQemuPrepareHostBackendChardevOne:
 * @dev: device definition object
 * @chardev: chardev source object
 * @opaque: Caller is expected to pass pointer to virDomainObj or NULL
 *
 * This helper sets up a chardev source backend for FD passing with fake
 * file descriptros. It's expected to be used as  callback for
 * 'qemuDomainDeviceBackendChardevForeach', thus the VM object is passed via
 * @opaque. Callers may pass NULL if the test scope is limited.
 */
int
testQemuPrepareHostBackendChardevOne(virDomainDeviceDef *dev,
                                     virDomainChrSourceDef *chardev,
                                     void *opaque)
{
    virDomainObj *vm = opaque;
    qemuDomainObjPrivate *priv = NULL;
    qemuDomainChrSourcePrivate *charpriv = QEMU_DOMAIN_CHR_SOURCE_PRIVATE(chardev);
    int fakesourcefd = -1;
    const char *devalias = NULL;

    if (vm)
        priv = vm->privateData;

    if (dev) {
        virDomainDeviceInfo *info = virDomainDeviceGetInfo(dev);
        devalias = info->alias;

        /* vhost-user disk doesn't use FD passing */
        if (dev->type == VIR_DOMAIN_DEVICE_DISK)
            return 0;

        if (dev->type == VIR_DOMAIN_DEVICE_NET) {
            /* due to a historical bug in qemu we don't use FD passtrhough for
             * vhost-sockets for network devices */
            return 0;
        }

        /* TPMs FD passing setup is special and handled separately */
        if (dev->type == VIR_DOMAIN_DEVICE_TPM)
            return 0;
    } else {
        devalias = "monitor";
    }

    switch ((virDomainChrType) chardev->type) {
    case VIR_DOMAIN_CHR_TYPE_NULL:
    case VIR_DOMAIN_CHR_TYPE_VC:
    case VIR_DOMAIN_CHR_TYPE_PTY:
    case VIR_DOMAIN_CHR_TYPE_DEV:
    case VIR_DOMAIN_CHR_TYPE_PIPE:
    case VIR_DOMAIN_CHR_TYPE_STDIO:
    case VIR_DOMAIN_CHR_TYPE_UDP:
    case VIR_DOMAIN_CHR_TYPE_TCP:
    case VIR_DOMAIN_CHR_TYPE_SPICEVMC:
    case VIR_DOMAIN_CHR_TYPE_SPICEPORT:
    case VIR_DOMAIN_CHR_TYPE_QEMU_VDAGENT:
    case VIR_DOMAIN_CHR_TYPE_DBUS:
        break;

    case VIR_DOMAIN_CHR_TYPE_FILE:
        fakesourcefd = 1750;

        if (fcntl(fakesourcefd, F_GETFD) != -1)
            abort();

        charpriv->sourcefd = qemuFDPassNew(devalias, priv);
        qemuFDPassAddFD(charpriv->sourcefd, &fakesourcefd, "-source");
        break;

    case VIR_DOMAIN_CHR_TYPE_UNIX:
        if (chardev->data.nix.listen) {
            g_autofree char *name = g_strdup_printf("%s-source", devalias);
            fakesourcefd = 1729;

            charpriv->directfd = qemuFDPassDirectNew(name, &fakesourcefd);
        }

        break;

    case VIR_DOMAIN_CHR_TYPE_NMDM:
    case VIR_DOMAIN_CHR_TYPE_LAST:
        break;
    }

    if (chardev->logfile) {
        int fd = 1751;

        if (fcntl(fd, F_GETFD) != -1)
            abort();

        charpriv->logfd = qemuFDPassNew(devalias, priv);

        qemuFDPassAddFD(charpriv->logfd, &fd, "-log");
    }

    return 0;
}
