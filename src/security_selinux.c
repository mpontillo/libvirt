/*
 * Copyright (C) 2008 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Authors:
 *     James Morris <jmorris@namei.org>
 *
 * SELinux security driver.
 */
#include <config.h>
#include <selinux/selinux.h>
#include <selinux/context.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "security.h"
#include "security_selinux.h"
#include "virterror_internal.h"
#include "util.h"
#include "memory.h"

static char default_domain_context[1024];
static char default_image_context[1024];
#define SECURITY_SELINUX_VOID_DOI       "0"
#define SECURITY_SELINUX_NAME "selinux"

/* TODO
   The data struct of used mcs should be replaced with a better data structure in the future
*/

struct MCS {
    char *mcs;
    struct MCS *next;
};
static struct MCS *mcsList = NULL;

static int
mcsAdd(const char *mcs)
{
    struct MCS *ptr;

    for (ptr = mcsList; ptr; ptr = ptr->next) {
        if (STREQ(ptr->mcs, mcs) == 0)
            return -1;
    }
    ptr = malloc(sizeof(struct MCS));
    ptr->mcs = strdup(mcs);
    ptr->next = mcsList;
    mcsList = ptr;
    return 0;
}

static int
mcsRemove(const char *mcs)
{
    struct MCS *prevptr = NULL;
    struct MCS *ptr = NULL;

    for (ptr = mcsList; ptr; ptr = ptr->next) {
        if (STREQ(ptr->mcs, mcs) == 0) {
            if (prevptr)
                prevptr->next = ptr->next;
            else {
                mcsList = ptr->next;
            }
            free(ptr->mcs);
            free(ptr);
            return 0;
        }
        prevptr = ptr;
    }
    return -1;
}

static char *
SELinuxGenNewContext(const char *oldcontext, const char *mcs)
{
    char *newcontext = NULL;
    char *scontext = strdup(oldcontext);
    if (!scontext) goto err;
    context_t con = context_new(scontext);
    if (!con) goto err;
    context_range_set(con, mcs);
    newcontext = strdup(context_str(con));
    context_free(con);
err:
    freecon(scontext);
    return (newcontext);
}

static int
SELinuxInitialize(virConnectPtr conn)
{
    char *ptr = NULL;
    int fd = 0;
    char ebuf[1024];

    virRandomInitialize(time(NULL) ^ getpid());

    fd = open(selinux_virtual_domain_context_path(), O_RDONLY);
    if (fd < 0) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: cannot open SELinux virtual domain context file %s: %s"),
                               __func__,selinux_virtual_domain_context_path(),
                               virStrerror(errno, ebuf, sizeof ebuf));
        return -1;
    }

    if (saferead(fd, default_domain_context, sizeof(default_domain_context)) < 0) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: cannot read SELinux virtual domain context file %s: %s"),
                               __func__,selinux_virtual_domain_context_path(),
                               virStrerror(errno, ebuf, sizeof ebuf));
        close(fd);
        return -1;
    }
    close(fd);

    ptr = strchrnul(default_domain_context, '\n');
    *ptr = '\0';

    if ((fd = open(selinux_virtual_image_context_path(), O_RDONLY)) < 0) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: cannot open SELinux virtual image context file %s: %s"),
                               __func__,selinux_virtual_image_context_path(),
                               virStrerror(errno, ebuf, sizeof ebuf));
        return -1;
    }

    if (saferead(fd, default_image_context, sizeof(default_image_context)) < 0) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: cannot read SELinux virtual image context file %s: %s"),
                               __func__,selinux_virtual_image_context_path(),
                               virStrerror(errno, ebuf, sizeof ebuf));
        close(fd);
        return -1;
    }
    close(fd);

    ptr = strchrnul(default_image_context, '\n');
    *ptr = '\0';

    return 0;
}

static int
SELinuxGenSecurityLabel(virDomainObjPtr vm)
{
    int rc = -1;
    char mcs[1024];
    char *scontext = NULL;
    int c1 = 0;
    int c2 = 0;
    if ( ( vm->def->seclabel.label ) ||
         ( vm->def->seclabel.model ) ||
         ( vm->def->seclabel.imagelabel ))
        return rc;

    do {
        c1 = virRandom(1024);
        c2 = virRandom(1024);

        if ( c1 == c2 ) {
            sprintf(mcs, "s0:c%d", c1);
        } else {
            if ( c1 == c2 )
                sprintf(mcs, "s0:c%d,c%d", c1, c2);
            else
                sprintf(mcs, "s0:c%d,c%d", c2, c1);
        }
    } while(mcsAdd(mcs) == -1);

    vm->def->seclabel.label = SELinuxGenNewContext(default_domain_context, mcs);
    if (! vm->def->seclabel.label)  goto err;
    vm->def->seclabel.imagelabel = SELinuxGenNewContext(default_image_context, mcs);
    if (! vm->def->seclabel.imagelabel)  goto err;
    vm->def->seclabel.model = strdup(SECURITY_SELINUX_NAME);
    if (! vm->def->seclabel.model) goto err;

    rc = 0;
    goto done;
err:
    free(vm->def->seclabel.label); vm->def->seclabel.label = NULL;
    free(vm->def->seclabel.imagelabel); vm->def->seclabel.imagelabel = NULL;
    free(vm->def->seclabel.model); vm->def->seclabel.model = NULL;
done:
    free(scontext);
    return rc;
}

static int
SELinuxSecurityDriverProbe(void)
{
    return is_selinux_enabled() ? SECURITY_DRIVER_ENABLE : SECURITY_DRIVER_DISABLE;
}

static int
SELinuxSecurityDriverOpen(virConnectPtr conn, virSecurityDriverPtr drv)
{
    /*
     * Where will the DOI come from?  SELinux configuration, or qemu
     * configuration? For the moment, we'll just set it to "0".
     */
    virSecurityDriverSetDOI(conn, drv, SECURITY_SELINUX_VOID_DOI);
    return SELinuxInitialize(conn);
}

static int
SELinuxGetSecurityLabel(virConnectPtr conn,
                        virDomainObjPtr vm,
                        virSecurityLabelPtr sec)
{
    security_context_t ctx;

    if (getpidcon(vm->pid, &ctx) == -1) {
        char ebuf[1024];
        virSecurityReportError(conn, VIR_ERR_ERROR, _("%s: error calling "
                               "getpidcon(): %s"), __func__,
                               virStrerror(errno, ebuf, sizeof ebuf));
        return -1;
    }

    if (strlen((char *) ctx) >= VIR_SECURITY_LABEL_BUFLEN) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: security label exceeds "
                               "maximum lenth: %d"), __func__,
                               VIR_SECURITY_LABEL_BUFLEN - 1);
        return -1;
    }

    strcpy(sec->label, (char *) ctx);
    free(ctx);

    sec->enforcing = security_getenforce();
    if (sec->enforcing == -1) {
        char ebuf[1024];
        virSecurityReportError(conn, VIR_ERR_ERROR, _("%s: error calling "
                               "security_getenforce(): %s"), __func__,
                               virStrerror(errno, ebuf, sizeof ebuf));
        return -1;
    }

    return 0;
}

static int
SELinuxSetFilecon(virConnectPtr conn, char *path, char *tcon)
{
    char ebuf[1024];

    if(setfilecon(path, tcon) < 0) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: unable to set security context "
                                 "'\%s\' on %s: %s."), __func__,
                               tcon,
                               path,
                               virStrerror(errno, ebuf, sizeof ebuf));
        if (security_getenforce() == 1)
            return -1;
    }
    return 0;
}

static int
SELinuxRestoreSecurityImageLabel(virConnectPtr conn,
                                 virDomainObjPtr vm,
                                 virDomainDeviceDefPtr dev)
{
    const virSecurityLabelDefPtr secdef = &vm->def->seclabel;

    if (secdef->imagelabel) {
        return SELinuxSetFilecon(conn, dev->data.disk->src, default_image_context);
    }
    return 0;
}

static int
SELinuxSetSecurityImageLabel(virConnectPtr conn,
                             virDomainObjPtr vm,
                             virDomainDeviceDefPtr dev)

{
    const virSecurityLabelDefPtr secdef = &vm->def->seclabel;

    if (secdef->imagelabel) {
        return SELinuxSetFilecon(conn, dev->data.disk->src, secdef->imagelabel);
    }
    return 0;
}

static int
SELinuxRestoreSecurityLabel(virConnectPtr conn,
                            virDomainObjPtr vm)
{
    const virSecurityLabelDefPtr secdef = &vm->def->seclabel;
    int i;
    int rc = 0;
    if (secdef->imagelabel) {
        for (i = 0 ; i < vm->def->ndisks ; i++) {
            if (SELinuxSetFilecon(conn, vm->def->disks[i]->src, default_image_context) < 0)
                rc = -1;
        }
        VIR_FREE(secdef->model);
        VIR_FREE(secdef->label);
        context_t con = context_new(secdef->imagelabel);
        if (con) {
            mcsRemove(context_range_get(con));
            context_free(con);
        }
        VIR_FREE(secdef->imagelabel);
    }
    return rc;
}

static int
SELinuxSetSecurityLabel(virConnectPtr conn,
                        virSecurityDriverPtr drv,
                        virDomainObjPtr vm)
{
    /* TODO: verify DOI */
    const virSecurityLabelDefPtr secdef = &vm->def->seclabel;
    int i;
    char ebuf[1024];

    if (!STREQ(drv->name, secdef->model)) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: security label driver mismatch: "
                                 "\'%s\' model configured for domain, but "
                                 "hypervisor driver is \'%s\'."),
                                 __func__, secdef->model, drv->name);
        if (security_getenforce() == 1)
                return -1;
    }

    if (setexeccon(secdef->label) == -1) {
        virSecurityReportError(conn, VIR_ERR_ERROR,
                               _("%s: unable to set security context "
                               "'\%s\': %s."), __func__, secdef->label,
                               virStrerror(errno, ebuf, sizeof ebuf));
        if (security_getenforce() == 1)
                return -1;
    }

    if (secdef->imagelabel) {
        for (i = 0 ; i < vm->def->ndisks ; i++) {
            if(setfilecon(vm->def->disks[i]->src, secdef->imagelabel) < 0) {
                virSecurityReportError(conn, VIR_ERR_ERROR,
                                       _("%s: unable to set security context "
                                         "'\%s\' on %s: %s."), __func__,
                                       secdef->imagelabel,
                                       vm->def->disks[i]->src,
                                       virStrerror(errno, ebuf, sizeof ebuf));
                if (security_getenforce() == 1)
                    return -1;
            }
        }
    }

    return 0;
}

virSecurityDriver virSELinuxSecurityDriver = {
    .name                       = SECURITY_SELINUX_NAME,
    .probe                      = SELinuxSecurityDriverProbe,
    .open                       = SELinuxSecurityDriverOpen,
    .domainSetSecurityImageLabel = SELinuxSetSecurityImageLabel,
    .domainRestoreSecurityImageLabel = SELinuxRestoreSecurityImageLabel,
    .domainGenSecurityLabel     = SELinuxGenSecurityLabel,
    .domainGetSecurityLabel     = SELinuxGetSecurityLabel,
    .domainRestoreSecurityLabel = SELinuxRestoreSecurityLabel,
    .domainSetSecurityLabel     = SELinuxSetSecurityLabel,
};
