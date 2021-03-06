/*
 *    HardInfo - Displays System Information
 *    Copyright (C) 2003-2017 Leandro A. F. Pereira <leandro@hardinfo.org>
 *    This file
 *    Copyright (C) 2018 Burt P. <pburt0@gmail.com>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, version 2.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "hardinfo.h"
#include "gpu_util.h"

nvgpu *nvgpu_new() {
    return g_new0(nvgpu, 1);
}

void *nvgpu_free(nvgpu *s) {
    if (s) {
        free(s->model);
        free(s->bios_version);
        free(s->uuid);
    }
}

static char *_line_value(char *line, const char *prefix) {
    if (g_str_has_prefix(g_strstrip(line), prefix)) {
        line += strlen(prefix) + 1;
        return g_strstrip(line);
    } else
        return NULL;
}

static gboolean nv_fill_procfs_info(gpud *s) {
    gchar *data, *p, *l, *next_nl;
    gchar *pci_loc = pci_address_str(s->pci_dev->domain, s->pci_dev->bus, s->pci_dev->device, s->pci_dev->function);
    gchar *nvi_file = g_strdup_printf("/proc/driver/nvidia/gpus/%s/information", pci_loc);

    g_file_get_contents(nvi_file, &data, NULL, NULL);
    g_free(pci_loc);
    g_free(nvi_file);

    if (data) {
        s->nv_info = nvgpu_new();
        p = data;
        while(next_nl = strchr(p, '\n')) {
            strend(p, '\n');
            g_strstrip(p);
            if (l = _line_value(p, "Model")) {
                s->nv_info->model = g_strdup(l);
                goto nv_details_next;
            }
            if (l = _line_value(p, "GPU UUID")) {
                s->nv_info->uuid = g_strdup(l);
                goto nv_details_next;
            }
            if (l = _line_value(p, "Video BIOS")) {
                s->nv_info->bios_version = g_strdup(l);
                goto nv_details_next;
            }

            /* TODO: more details */

            nv_details_next:
                p = next_nl + 1;
        }
        g_free(data);
        return TRUE;
    }
    return FALSE;
}

gpud *gpud_new() {
    return g_new0(gpud, 1);
}

void gpud_free(gpud *s) {
    if (s) {
        free(s->id);
        free(s->nice_name);
        free(s->vendor_str);
        free(s->device_str);
        free(s->location);
        free(s->drm_dev);
        free(s->sysfs_drm_path);
        free(s->dt_compat);
        pcid_free(s->pci_dev);
        nvgpu_free(s->nv_info);
        g_free(s);
    }
}

void gpud_list_free(gpud *s) {
    gpud *n;
    while(s != NULL) {
        n = s->next;
        gpud_free(s);
        s = n;
    }
}

/* returns number of items after append */
static int gpud_list_append(gpud *l, gpud *n) {
    int c = 0;
    while(l != NULL) {
        c++;
        if (l->next == NULL) {
            if (n != NULL) {
                l->next = n;
                c++;
            }
            break;
        }
        l = l->next;
    }
    return c;
}

int gpud_list_count(gpud *s) {
    return gpud_list_append(s, NULL);
}

/* TODO: In the future, when there is more vendor specific information available in
 * the gpu struct, then more precise names can be given to each gpu */
static void make_nice_name(gpud *s) {

    /* NV information available */
    if (s->nv_info && s->nv_info->model) {
        s->nice_name = g_strdup_printf("%s %s", "NVIDIA", s->nv_info->model);
        return;
    }

    static const char unk_v[] = "Unknown"; /* do not...    */
    static const char unk_d[] = "Device";  /* ...translate */
    const char *vendor_str = s->vendor_str;
    const char *device_str = s->device_str;
    if (!vendor_str)
        vendor_str = unk_v;
    if (!device_str)
        device_str = unk_d;

    if (strstr(vendor_str, "NVIDIA")) {
        /* nvidia PCI strings are pretty nice already,
         * just shorten the company name */
        s->nice_name = g_strdup_printf("%s %s", "NVIDIA", device_str);
    } else if (strstr(vendor_str, "AMD/ATI")) {
        /* AMD PCI strings are crazy stupid because they use the exact same
         * chip and device id for a zillion "different products" */
        char *full_name = strdup(device_str);
        /* Try and shorten it to the chip code name only */
        char *b = strchr(full_name, '[');
        if (b) *b = '\0';
        s->nice_name = g_strdup_printf("%s %s", "AMD/ATI", g_strstrip(full_name));
        free(full_name);
    } else {
        /* nothing nicer */
        s->nice_name = g_strdup_printf("%s %s", vendor_str, device_str);
    }

}

gpud *dt_soc_gpu() {
    static const char std_soc_gpu_drm_path[] = "/sys/devices/platform/soc/soc:gpu/drm";

    /* compatible contains a list of compatible hardware, so be careful
     * with matching order.
     * ex: "ti,omap3-beagleboard-xm", "ti,omap3450", "ti,omap3";
     * matches "omap3 family" first.
     * ex: "brcm,bcm2837", "brcm,bcm2836";
     * would match 2836 when it is a 2837.
     */
    const struct {
        char *search_str;
        char *vendor;
        char *soc;
    } dt_compat_searches[] = {
        { "brcm,bcm2837-vc4", "Broadcom", "VideoCore IV" },
        { "brcm,bcm2836-vc4", "Broadcom", "VideoCore IV" },
        { "brcm,bcm2835-vc4", "Broadcom", "VideoCore IV" },
        { NULL, NULL }
    };
    char *compat = NULL;
    char *vendor = NULL, *device = NULL;
    int i;

    gpud *gpu = NULL;

    compat = dtr_get_string("/soc/gpu/compatible", 1);
    if (compat == NULL) return NULL;

    gpu = gpud_new();

    i = 0;
    while(dt_compat_searches[i].search_str != NULL) {
        if (strstr(compat, dt_compat_searches[i].search_str) != NULL) {
            vendor = dt_compat_searches[i].vendor;
            device = dt_compat_searches[i].soc;
            break;
        }
        i++;
    }

    gpu->dt_compat = compat;
    gpu->dt_vendor = vendor;
    gpu->dt_device = device;

    gpu->id = strdup("dt-soc-gpu");
    gpu->location = strdup("SOC");

    if (access(std_soc_gpu_drm_path, F_OK) != -1)
        gpu->sysfs_drm_path = strdup(std_soc_gpu_drm_path);
    if (vendor) gpu->vendor_str = strdup(vendor);
    if (device) gpu->device_str = strdup(device);
    make_nice_name(gpu);

    return gpu;
}

gpud *gpu_get_device_list() {
    int cn = 0;
    gpud *list = NULL;

/* Can we just ask DRM someway? ... */

/* Try PCI ... */
    pcid *pci_list = pci_get_device_list(0x300,0x3ff);
    pcid *curr = pci_list;

    int c = pcid_list_count(pci_list);

    if (c > 0) {
        while(curr) {
            char *pci_loc = NULL;
            gpud *new_gpu = gpud_new();
            new_gpu->pci_dev = curr;

            pci_loc = pci_address_str(curr->domain, curr->bus, curr->device, curr->function);

            int len;
            char drm_id[512] = "", card_id[64] = "";
            char *drm_dev = NULL;
            gchar *drm_path =
                g_strdup_printf("/dev/dri/by-path/pci-%s-card", pci_loc);
            memset(drm_id, 0, 512);
            if ((len = readlink(drm_path, drm_id, sizeof(drm_id)-1)) != -1)
                drm_id[len] = '\0';
            g_free(drm_path);

            if (strlen(drm_id) != 0) {
                /* drm has the card */
                drm_dev = strstr(drm_id, "card");
                if (drm_dev)
                    snprintf(card_id, 64, "drm-%s", drm_dev);
            }

            if (strlen(card_id) == 0) {
                /* fallback to our own counter */
                snprintf(card_id, 64, "pci-dc%d", cn);
                cn++;
            }

            if (drm_dev)
                new_gpu->drm_dev = strdup(drm_dev);

            char *sysfs_path_candidate = g_strdup_printf("%s/%s/drm", SYSFS_PCI_ROOT, pci_loc);
            if (access(sysfs_path_candidate, F_OK) != -1) {
                new_gpu->sysfs_drm_path = sysfs_path_candidate;
            } else
                free(sysfs_path_candidate);
            new_gpu->location = g_strdup_printf("PCI/%s", pci_loc);
            new_gpu->id = strdup(card_id);
            if (curr->vendor_id_str) new_gpu->vendor_str = strdup(curr->vendor_id_str);
            if (curr->device_id_str) new_gpu->device_str = strdup(curr->device_id_str);
            nv_fill_procfs_info(new_gpu);
            make_nice_name(new_gpu);
            if (list == NULL)
                list = new_gpu;
            else
                gpud_list_append(list, new_gpu);

            free(pci_loc);
            curr=curr->next;
        }

        /* don't pcid_list_free(pci_list); They will be freed by gpud_free() */
        return list;
    }

/* Try Device Tree ... */
    list = dt_soc_gpu();
    if (list) return list;

/* Try other things ... */


}


