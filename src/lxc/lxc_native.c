/*
 * lxc_native.c: LXC native configuration import
 *
 * Copyright (c) 2013 SUSE LINUX Products GmbH, Nuernberg, Germany.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Author: Cedric Bosdonnat <cbosdonnat@suse.com>
 */

#include <config.h>
#include <stdio.h>

#include "internal.h"
#include "lxc_container.h"
#include "lxc_native.h"
#include "util/viralloc.h"
#include "util/virfile.h"
#include "util/virlog.h"
#include "util/virstring.h"
#include "util/virconf.h"

#define VIR_FROM_THIS VIR_FROM_LXC


static virDomainFSDefPtr
lxcCreateFSDef(int type,
               const char *src,
               const char* dst,
               bool readonly,
               unsigned long long usage)
{
    virDomainFSDefPtr def;

    if (VIR_ALLOC(def) < 0)
        return NULL;

    def->type = type;
    def->accessmode = VIR_DOMAIN_FS_ACCESSMODE_PASSTHROUGH;
    if (src && VIR_STRDUP(def->src, src) < 0)
        goto error;
    if (VIR_STRDUP(def->dst, dst) < 0)
        goto error;
    def->readonly = readonly;
    def->usage = usage;

    return def;

 error:
    virDomainFSDefFree(def);
    return NULL;
}

typedef struct _lxcFstab lxcFstab;
typedef lxcFstab *lxcFstabPtr;
struct _lxcFstab {
    lxcFstabPtr next;
    char *src;
    char *dst;
    char *type;
    char *options;
};

static void
lxcFstabFree(lxcFstabPtr fstab)
{
    while (fstab) {
        lxcFstabPtr next = NULL;
        next = fstab->next;

        VIR_FREE(fstab->src);
        VIR_FREE(fstab->dst);
        VIR_FREE(fstab->type);
        VIR_FREE(fstab->options);
        VIR_FREE(fstab);

        fstab = next;
    }
}

static char ** lxcStringSplit(const char *string)
{
    char *tmp;
    size_t i;
    size_t ntokens = 0;
    char **parts;
    char **result = NULL;

    if (VIR_STRDUP(tmp, string) < 0)
        return NULL;

    /* Replace potential \t by a space */
    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == '\t')
            tmp[i] = ' ';
    }

    if (!(parts = virStringSplit(tmp, " ", 0)))
        goto error;

    /* Append NULL element */
    if (VIR_EXPAND_N(result, ntokens, 1) < 0)
        goto error;

    for (i = 0; parts[i]; i++) {
        if (STREQ(parts[i], ""))
            continue;

        if (VIR_EXPAND_N(result, ntokens, 1) < 0)
            goto error;

        if (VIR_STRDUP(result[ntokens-2], parts[i]) < 0)
            goto error;
    }

    VIR_FREE(tmp);
    virStringFreeList(parts);
    return result;

error:
    VIR_FREE(tmp);
    virStringFreeList(parts);
    virStringFreeList(result);
    return NULL;
}

static lxcFstabPtr
lxcParseFstabLine(char *fstabLine)
{
    lxcFstabPtr fstab = NULL;
    char **parts;

    if (!fstabLine || VIR_ALLOC(fstab) < 0)
        return NULL;

    if (!(parts = lxcStringSplit(fstabLine)))
        goto error;

    if (!parts[0] || !parts[1] || !parts[2] || !parts[3])
        goto error;

    if (VIR_STRDUP(fstab->src, parts[0]) < 0 ||
            VIR_STRDUP(fstab->dst, parts[1]) < 0 ||
            VIR_STRDUP(fstab->type, parts[2]) < 0 ||
            VIR_STRDUP(fstab->options, parts[3]) < 0)
        goto error;

    virStringFreeList(parts);

    return fstab;

error:
    lxcFstabFree(fstab);
    virStringFreeList(parts);
    return NULL;
}

static int
lxcAddFSDef(virDomainDefPtr def,
            int type,
            const char *src,
            const char *dst,
            bool readonly,
            unsigned long long usage)
{
    virDomainFSDefPtr fsDef = NULL;

    if (!(fsDef = lxcCreateFSDef(type, src, dst, readonly, usage)))
        goto error;

    if (VIR_EXPAND_N(def->fss, def->nfss, 1) < 0)
        goto error;
    def->fss[def->nfss - 1] = fsDef;

    return 0;

error:
    virDomainFSDefFree(fsDef);
    return -1;
}

static int
lxcSetRootfs(virDomainDefPtr def,
             virConfPtr properties)
{
    int type = VIR_DOMAIN_FS_TYPE_MOUNT;
    virConfValuePtr value;

    if (!(value = virConfGetValue(properties, "lxc.rootfs")) ||
        !value->str) {
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                       _("Missing lxc.rootfs configuration"));
        return -1;
    }

    if (STRPREFIX(value->str, "/dev/"))
        type = VIR_DOMAIN_FS_TYPE_BLOCK;

    if (lxcAddFSDef(def, type, value->str, "/", false, 0) < 0)
        return -1;

    return 0;
}

static int
lxcConvertSize(const char *size, unsigned long long *value)
{
    char *unit = NULL;

    /* Split the string into value and unit */
    if (virStrToLong_ull(size, &unit, 10, value) < 0)
        goto error;

    if (STREQ(unit, "%")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("can't convert relative size: '%s'"),
                       size);
        return -1;
    } else {
        if (virScaleInteger(value, unit, 1, ULLONG_MAX) < 0)
            goto error;
    }

    return 0;

error:
    virReportError(VIR_ERR_INTERNAL_ERROR,
                   _("failed to convert size: '%s'"),
                   size);
    return -1;
}

static int
lxcAddFstabLine(virDomainDefPtr def, lxcFstabPtr fstab)
{
    const char *src = NULL;
    char *dst = NULL;
    char **options = virStringSplit(fstab->options, ",", 0);
    bool readonly;
    int type = VIR_DOMAIN_FS_TYPE_MOUNT;
    unsigned long long usage = 0;
    int ret = -1;

    if (!options)
        return -1;

    if (fstab->dst[0] != '/') {
        if (virAsprintf(&dst, "/%s", fstab->dst) < 0)
            goto cleanup;
    } else {
        if (VIR_STRDUP(dst, fstab->dst) < 0)
            goto cleanup;
    }

    /* Check that we don't add basic mounts */
    if (lxcIsBasicMountLocation(dst)) {
        ret = 0;
        goto cleanup;
    }

    if (STREQ(fstab->type, "tmpfs")) {
        char *sizeStr = NULL;
        size_t i;
        type = VIR_DOMAIN_FS_TYPE_RAM;

        for (i = 0; options[i]; i++) {
            if ((sizeStr = STRSKIP(options[i], "size="))) {
                if (lxcConvertSize(sizeStr, &usage) < 0)
                    goto cleanup;
                break;
            }
        }
        if (!sizeStr) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("missing tmpfs size, set the size option"));
            goto cleanup;
        }
    } else {
        src = fstab->src;
    }

    /* Do we have ro in options? */
    readonly = virStringArrayHasString(options, "ro");

    if (lxcAddFSDef(def, type, src, dst, readonly, usage) < 0)
        goto cleanup;

    ret = 1;

 cleanup:
    VIR_FREE(dst);
    virStringFreeList(options);
    return ret;
}

static int
lxcFstabWalkCallback(const char* name, virConfValuePtr value, void * data)
{
    int ret = 0;
    lxcFstabPtr fstabLine;
    virDomainDefPtr def = data;

    /* We only care about lxc.mount.entry lines */
    if (STRNEQ(name, "lxc.mount.entry"))
        return 0;

    fstabLine = lxcParseFstabLine(value->str);

    if (!fstabLine)
        return -1;

    if (lxcAddFstabLine(def, fstabLine) < 0)
        ret = -1;

    lxcFstabFree(fstabLine);
    return ret;
}

typedef struct {
    char *type;
    bool privnet;
    size_t networks;
} lxcNetworkParseData;

static int
lxcNetworkWalkCallback(const char *name, virConfValuePtr value, void *data)
{
    lxcNetworkParseData *parseData = data;

    if (STREQ(name, "lxc.network.type")) {
        if (parseData->type != NULL && STREQ(parseData->type, "none"))
            parseData->privnet = false;
        else if ((parseData->type != NULL) &&
                        STRNEQ(parseData->type, "empty") &&
                        STRNEQ(parseData->type, "")) {
            parseData->networks++;
        }

        /* Start a new network interface config */
        parseData->type = NULL;

        /* Keep the new value */
        parseData->type = value->str;
    }
    return 0;
}

static int
lxcConvertNetworkSettings(virDomainDefPtr def, virConfPtr properties)
{
    lxcNetworkParseData data = {NULL, true, 0};

    virConfWalk(properties, lxcNetworkWalkCallback, &data);

    if ((data.type != NULL) && STREQ(data.type, "none"))
        data.privnet = false;
    else if ((data.type != NULL) && STRNEQ(data.type, "empty") &&
                    STRNEQ(data.type, "")) {
        data.networks++;
    }

    if (data.networks == 0 && data.privnet) {
        /* When no network type is provided LXC only adds loopback */
        def->features[VIR_DOMAIN_FEATURE_PRIVNET] = VIR_DOMAIN_FEATURE_STATE_ON;
    }

    return 0;
}

virDomainDefPtr
lxcParseConfigString(const char *config)
{
    virDomainDefPtr vmdef = NULL;
    virConfPtr properties = NULL;
    virConfValuePtr value;

    if (!(properties = virConfReadMem(config, 0, VIR_CONF_FLAG_LXC_FORMAT)))
        return NULL;

    if (VIR_ALLOC(vmdef) < 0)
        goto error;

    if (virUUIDGenerate(vmdef->uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("failed to generate uuid"));
        goto error;
    }

    vmdef->id = -1;
    vmdef->mem.max_balloon = 64 * 1024;

    vmdef->onReboot = VIR_DOMAIN_LIFECYCLE_RESTART;
    vmdef->onCrash = VIR_DOMAIN_LIFECYCLE_DESTROY;
    vmdef->onPoweroff = VIR_DOMAIN_LIFECYCLE_DESTROY;
    vmdef->virtType = VIR_DOMAIN_VIRT_LXC;

    /* Value not handled by the LXC driver, setting to
     * minimum required to make XML parsing pass */
    vmdef->maxvcpus = 1;

    vmdef->nfss = 0;

    if (VIR_STRDUP(vmdef->os.type, "exe") < 0)
        goto error;

    if (VIR_STRDUP(vmdef->os.init, "/sbin/init") < 0)
        goto error;

    if (!(value = virConfGetValue(properties, "lxc.utsname")) ||
            !value->str || (VIR_STRDUP(vmdef->name, value->str) < 0))
        goto error;
    if (!vmdef->name && (VIR_STRDUP(vmdef->name, "unnamed") < 0))
        goto error;

    if (lxcSetRootfs(vmdef, properties) < 0)
        goto error;

    /* Look for fstab: we shouldn't have it */
    if (virConfGetValue(properties, "lxc.mount")) {
        virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED, "%s",
                       _("lxc.mount found, use lxc.mount.entry lines instead"));
        goto error;
    }

    /* Loop over lxc.mount.entry to add filesystem devices for them */
    value = virConfGetValue(properties, "lxc.mount.entry");
    if (virConfWalk(properties, lxcFstabWalkCallback, vmdef) < 0)
        goto error;

    /* Network configuration */
    if (lxcConvertNetworkSettings(vmdef, properties) < 0)
        goto error;

    goto cleanup;

error:
    virDomainDefFree(vmdef);
    vmdef = NULL;

cleanup:
    virConfFree(properties);

    return vmdef;
}
