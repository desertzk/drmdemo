#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

struct buffer_object
{
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t size;
    uint8_t *vaddr;
    uint32_t fb_id;
};

struct buffer_object buf[2];
static int terminate;

/*
 * A new struct is introduced: drm_object. It stores properties of certain
 * objects (connectors, CRTC and planes) that are used in atomic modeset setup
 * and also in atomic page-flips (all planes updated in a single IOCTL).
 */

struct drm_object
{
    drmModeObjectProperties *props;
    drmModePropertyRes **props_info;
    uint32_t id;
};

struct modeset_output
{
    struct modeset_output *next;

    unsigned int front_buf;
    struct buffer_object bufs[2];

    struct drm_object connector;
    struct drm_object crtc;
    struct drm_object plane;

    drmModeModeInfo mode;
    uint32_t mode_blob_id;
    uint32_t crtc_index;

    bool pflip_pending;
    bool cleanup;

    uint8_t r, g, b;
    bool r_up, g_up, b_up;
};

static struct modeset_output *output_list = NULL;
/*
 * get_drm_object_properties() is a new helpfer function that retrieves
 * the properties of a certain CRTC, plane or connector object.
 */

static void modeset_get_object_properties(int fd, struct drm_object *obj,
                                          uint32_t type)
{
    const char *type_str;
    unsigned int i;

    obj->props = drmModeObjectGetProperties(fd, obj->id, type);
    if (!obj->props)
    {
        switch (type)
        {
        case DRM_MODE_OBJECT_CONNECTOR:
            type_str = "connector";
            break;
        case DRM_MODE_OBJECT_PLANE:
            type_str = "plane";
            break;
        case DRM_MODE_OBJECT_CRTC:
            type_str = "CRTC";
            break;
        default:
            type_str = "unknown type";
            break;
        }
        fprintf(stderr, "cannot get %s %d properties: %s\n",
                type_str, obj->id, strerror(errno));
        return;
    }

    obj->props_info = calloc(obj->props->count_props, sizeof(obj->props_info));
    for (i = 0; i < obj->props->count_props; i++)
        obj->props_info[i] = drmModeGetProperty(fd, obj->props->props[i]);
}

static int modeset_create_fb(int fd, struct buffer_object *bo, uint32_t color)
{
    struct drm_mode_create_dumb create = {};
    struct drm_mode_map_dumb map = {};

    create.width = bo->width;
    create.height = bo->height;
    create.bpp = 32;
    drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);

    bo->pitch = create.pitch;
    bo->size = create.size;
    bo->handle = create.handle;
    // drmModeAddFB(fd, bo->width, bo->height, 24, 32, bo->pitch,
    // 		   bo->handle, &bo->fb_id);
    uint32_t handles[4] = {bo->handle};
    uint32_t pitches[4] = {bo->pitch};
    uint32_t offsets[4] = {0};
    uint32_t format = DRM_FORMAT_XRGB8888; // Explicit format

    drmModeAddFB2(fd, bo->width, bo->height, format, handles, pitches, offsets, &bo->fb_id, 0);

    map.handle = create.handle;
    drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);

    bo->vaddr = mmap(0, create.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, map.offset);

    // memset(bo->vaddr, 0xf0, bo->size);
    uint32_t *pixel = (uint32_t *)bo->vaddr;
    for (size_t i = 0; i < (bo->size / 4); i++)
    {
        pixel[i] = color; // Solid red
    }

    return 0;
}

static void modeset_destroy_fb(int fd, struct buffer_object *bo)
{
    struct drm_mode_destroy_dumb destroy = {};

    drmModeRmFB(fd, bo->fb_id);

    munmap(bo->vaddr, bo->size);

    destroy.handle = bo->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

static uint32_t get_property_id(int fd, drmModeObjectProperties *props,
                                const char *name)
{
    drmModePropertyPtr property;
    uint32_t i, id = 0;

    for (i = 0; i < props->count_props; i++)
    {
        property = drmModeGetProperty(fd, props->props[i]);
        if (!strcmp(property->name, name))
            id = property->prop_id;
        drmModeFreeProperty(property);

        if (id)
            break;
    }

    return id;
}

/*
 * get_property_value() is a new function. Given a device, the properties of
 * an object and a name, search for the value of property 'name'. If we can't
 * find it, return -1.
 */

static int64_t get_property_value(int fd, drmModeObjectPropertiesPtr props,
                                  const char *name)
{
    drmModePropertyPtr prop;
    uint64_t value;
    bool found;
    int j;

    found = false;
    for (j = 0; j < props->count_props && !found; j++)
    {
        prop = drmModeGetProperty(fd, props->props[j]);
        if (!strcmp(prop->name, name))
        {
            value = props->prop_values[j];
            found = true;
        }
        drmModeFreeProperty(prop);
    }

    if (!found)
        return -1;
    return value;
}

/*
 * modeset_find_crtc() changes a little bit. Now we also have to save the CRTC
 * index, and not only its id.
 */

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
                             struct modeset_output *out)
{
    drmModeEncoder *enc;
    unsigned int i, j;
    uint32_t crtc;
    struct modeset_output *iter;

    /* first try the currently conected encoder+crtc */
    if (conn->encoder_id)
        enc = drmModeGetEncoder(fd, conn->encoder_id);
    else
        enc = NULL;

    if (enc)
    {
        if (enc->crtc_id)
        {
            crtc = enc->crtc_id;
            for (iter = output_list; iter; iter = iter->next)
            {
                if (iter->crtc.id == crtc)
                {
                    crtc = 0;
                    break;
                }
            }

            if (crtc > 0)
            {
                drmModeFreeEncoder(enc);
                out->crtc.id = crtc;
                /* find the CRTC's index */
                for (i = 0; i < res->count_crtcs; ++i)
                {
                    if (res->crtcs[i] == crtc)
                    {
                        out->crtc_index = i;
                        break;
                    }
                }
                return 0;
            }
        }

        drmModeFreeEncoder(enc);
    }

    /* If the connector is not currently bound to an encoder or if the
     * encoder+crtc is already used by another connector (actually unlikely
     * but lets be safe), iterate all other available encoders to find a
     * matching CRTC.
     */
    for (i = 0; i < conn->count_encoders; ++i)
    {
        enc = drmModeGetEncoder(fd, conn->encoders[i]);
        if (!enc)
        {
            fprintf(stderr, "cannot retrieve encoder %u:%u (%d): %m\n",
                    i, conn->encoders[i], errno);
            continue;
        }

        /* iterate all global CRTCs */
        for (j = 0; j < res->count_crtcs; ++j)
        {
            /* check whether this CRTC works with the encoder */
            if (!(enc->possible_crtcs & (1 << j)))
                continue;

            /* check that no other output already uses this CRTC */
            crtc = res->crtcs[j];
            for (iter = output_list; iter; iter = iter->next)
            {
                if (iter->crtc.id == crtc)
                {
                    crtc = 0;
                    break;
                }
            }

            /* We have found a CRTC, so save it and return. Note
             * that we have to save its index as well. The CRTC
             * index (not its ID) will be used when searching for a
             * suitable plane.
             */
            if (crtc > 0)
            {
                fprintf(stdout, "crtc %u found for encoder %u, will need full modeset\n",
                        crtc, conn->encoders[i]);
                ;
                drmModeFreeEncoder(enc);
                out->crtc.id = crtc;
                out->crtc_index = j;
                return 0;
            }
        }

        drmModeFreeEncoder(enc);
    }

    fprintf(stderr, "cannot find suitable crtc for connector %u\n",
            conn->connector_id);
    return -ENOENT;
}

/*
 * modeset_find_plane() is a new function. Given a certain combination
 * of connector+CRTC, it looks for a primary plane for it.
 */

static int modeset_find_plane(int fd, struct modeset_output *out)
{
    drmModePlaneResPtr plane_res;
    bool found_primary = false;
    int i, ret = -EINVAL;

    plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res)
    {
        fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
                strerror(errno));
        return -ENOENT;
    }

    /* iterates through all planes of a certain device */
    for (i = 0; (i < plane_res->count_planes) && !found_primary; i++)
    {
        int plane_id = plane_res->planes[i];

        drmModePlanePtr plane = drmModeGetPlane(fd, plane_id);
        if (!plane)
        {
            fprintf(stderr, "drmModeGetPlane(%u) failed: %s\n", plane_id,
                    strerror(errno));
            continue;
        }

        /* check if the plane can be used by our CRTC */
        if (plane->possible_crtcs & (1 << out->crtc_index))
        {
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);

            /* Get the "type" property to check if this is a primary
             * plane. Type property is special, as its enum value is
             * defined in UAPI headers. For the properties that are
             * not defined in the UAPI headers, we would have to
             * give kernel the property name and it would return the
             * corresponding enum value. We could also do this for
             * the "type" property, but it would make this simple
             * example more complex. The reason why defining enum
             * values for kernel properties in UAPI headers is
             * deprecated is that string names are easier to both
             * (userspace and kernel) make unique and keep
             * consistent between drivers and kernel versions. But
             * in order to not break userspace, some properties were
             * left in the UAPI headers as well.
             */
            if (get_property_value(fd, props, "type") == DRM_PLANE_TYPE_PRIMARY)
            {
                found_primary = true;
                out->plane.id = plane_id;
                ret = 0;
            }

            drmModeFreeObjectProperties(props);
        }

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(plane_res);

    if (found_primary)
        fprintf(stdout, "found primary plane, id: %d\n", out->plane.id);
    else
        fprintf(stdout, "couldn't find a primary plane\n");
    return ret;
}

/*
 * modeset_drm_object_fini() is a new helper function that destroys CRTCs,
 * connectors and planes
 */

static void modeset_drm_object_fini(struct drm_object *obj)
{
    for (int i = 0; i < obj->props->count_props; i++)
        drmModeFreeProperty(obj->props_info[i]);
    free(obj->props_info);
    drmModeFreeObjectProperties(obj->props);
}

/*
 * modeset_setup_objects() is a new function. It helps us to retrieve
 * connector, CRTC and plane objects properties from the device. These
 * properties will help us during the atomic modesetting commit, so we save
 * them in our struct modeset_output object.
 */

static int modeset_setup_objects(int fd, struct modeset_output *out)
{
    struct drm_object *connector = &out->connector;
    struct drm_object *crtc = &out->crtc;
    struct drm_object *plane = &out->plane;

    /* retrieve connector properties from the device */
    modeset_get_object_properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR);
    if (!connector->props)
        goto out_conn;

    /* retrieve CRTC properties from the device */
    modeset_get_object_properties(fd, crtc, DRM_MODE_OBJECT_CRTC);
    if (!crtc->props)
        goto out_crtc;

    /* retrieve plane properties from the device */
    modeset_get_object_properties(fd, plane, DRM_MODE_OBJECT_PLANE);
    if (!plane->props)
        goto out_plane;

    return 0;

out_plane:
    modeset_drm_object_fini(crtc);
out_crtc:
    modeset_drm_object_fini(connector);
out_conn:
    return -ENOMEM;
}

/*
 * modeset_destroy_objects() is a new function. It destroys what we allocate
 * in modeset_setup_objects().
 */

static void modeset_destroy_objects(int fd, struct modeset_output *out)
{
    modeset_drm_object_fini(&out->connector);
    modeset_drm_object_fini(&out->crtc);
    modeset_drm_object_fini(&out->plane);
}

/*
 * With a certain combination of connector+CRTC, we look for a suitable primary
 * plane for it. After that, we retrieve connector, CRTC and plane objects
 * properties from the device. These objects are used during the atomic modeset
 * setup (see modeset_atomic_prepare_commit()) and also during the page-flips
 * (see modeset_draw_out() and modeset_atomic_commit()).
 *
 * Besides that, we have to create a blob property that receives the output
 * mode. When we perform an atomic commit, the driver expects a CRTC property
 * named "MODE_ID", which points to the id of a blob. This usually happens for
 * properties that are not simple types. In this particular case, out->mode is a
 * struct. But we could have another property that expects the id of a blob that
 * holds an array, for instance.
 */

static struct modeset_output *modeset_output_create(int fd, drmModeRes *res,
                                                    drmModeConnector *conn)
{
    int ret;
    struct modeset_output *out;

    /* creates an output structure */
    out = malloc(sizeof(*out));
    memset(out, 0, sizeof(*out));
    out->connector.id = conn->connector_id;

    /* check if a monitor is connected */
    if (conn->connection != DRM_MODE_CONNECTED)
    {
        fprintf(stderr, "ignoring unused connector %u\n",
                conn->connector_id);
        goto out_error;
    }

    /* check if there is at least one valid mode */
    if (conn->count_modes == 0)
    {
        fprintf(stderr, "no valid mode for connector %u\n",
                conn->connector_id);
        goto out_error;
    }

    /* copy the mode information into our output structure */
    memcpy(&out->mode, &conn->modes[0], sizeof(out->mode));
    /* create the blob property using out->mode and save its id in the output*/
    if (drmModeCreatePropertyBlob(fd, &out->mode, sizeof(out->mode),
                                  &out->mode_blob_id) != 0)
    {
        fprintf(stderr, "couldn't create a blob property\n");
        goto out_error;
    }
    fprintf(stderr, "mode for connector %u is %ux%u\n",
            conn->connector_id, out->bufs[0].width, out->bufs[0].height);

    /* find a crtc for this connector */
    ret = modeset_find_crtc(fd, res, conn, out);
    if (ret)
    {
        fprintf(stderr, "no valid crtc for connector %u\n",
                conn->connector_id);
        goto out_blob;
    }

    /* with a connector and crtc, find a primary plane */
    ret = modeset_find_plane(fd, out);
    if (ret)
    {
        fprintf(stderr, "no valid plane for crtc %u\n", out->crtc.id);
        goto out_blob;
    }

    /* gather properties of our connector, CRTC and planes */
    ret = modeset_setup_objects(fd, out);
    if (ret)
    {
        fprintf(stderr, "cannot get plane properties\n");
        goto out_blob;
    }

    /* setup front/back framebuffers for this CRTC */
    // ret = modeset_setup_framebuffers(fd, conn, out);
    // if (ret) {
    // fprintf(stderr, "cannot create framebuffers for connector %u\n",
    // conn->connector_id);
    // goto out_obj;
    // }

    return out;

out_obj:
    modeset_destroy_objects(fd, out);
out_blob:
    drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
out_error:
    free(out);
    return NULL;
}

/*
 * modeset_open() changes just a little bit. We now have to set that we're going
 * to use the KMS atomic API and check if the device is capable of handling it.
 */

static int modeset_open(int *out, const char *node)
{
    int fd, ret;
    uint64_t cap;

    fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
    {
        ret = -errno;
        fprintf(stderr, "cannot open '%s': %m\n", node);
        return ret;
    }

    /* Set that we want to receive all the types of planes in the list. This
     * have to be done since, for legacy reasons, the default behavior is to
     * expose only the overlay planes to the users. The atomic API only
     * works if this is set.
     */
    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (ret)
    {
        fprintf(stderr, "failed to set universal planes cap, %d\n", ret);
        return ret;
    }

    /* Here we set that we're going to use the KMS atomic API. It's supposed
     * to set the DRM_CLIENT_CAP_UNIVERSAL_PLANES automatically, but it's a
     * safe behavior to set it explicitly as we did in the previous
     * commands. This is also good for learning purposes.
     */
    ret = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (ret)
    {
        fprintf(stderr, "failed to set atomic cap, %d", ret);
        return ret;
    }

    if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap)
    {
        fprintf(stderr, "drm device '%s' does not support dumb buffers\n",
                node);
        close(fd);
        return -EOPNOTSUPP;
    }

    if (drmGetCap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) < 0 || !cap)
    {
        fprintf(stderr, "drm device '%s' does not support atomic KMS\n",
                node);
        close(fd);
        return -EOPNOTSUPP;
    }

    *out = fd;
    return 0;
}

uint32_t plane_id;
uint32_t prop_fb_id;
uint32_t prop_crtc_id;
uint32_t prop_src_x;
uint32_t prop_src_y;
uint32_t prop_src_w;
uint32_t prop_src_h;
uint32_t prop_crtc_x;
uint32_t prop_crtc_y;
uint32_t prop_crtc_w;
uint32_t prop_crtc_h;

// handle vblank(vsync) event
static void modeset_page_flip_handler(int fd, uint32_t frame,
                                      uint32_t sec, uint32_t usec,
                                      void *data)
{
    static int i = 0;
    uint32_t crtc_id = *(uint32_t *)data;

    i ^= 1;
    printf("fd %d crtc_id %d buf[i].fb_id %d data %llx \n", fd, crtc_id, buf[i].fb_id, data);
    // drmModePageFlip(fd, crtc_id, buf[i].fb_id,
    // 		DRM_MODE_PAGE_FLIP_EVENT, data);

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, prop_fb_id, buf[i].fb_id);
    drmModeAtomicAddProperty(req, plane_id, prop_src_x, 0);
    drmModeAtomicAddProperty(req, plane_id, prop_src_y, 0);
    drmModeAtomicAddProperty(req, plane_id, prop_src_w, buf[i].width << 16); // Full buffer
    drmModeAtomicAddProperty(req, plane_id, prop_src_h, buf[i].height << 16);
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_x, 0); // Start at top-left
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_y, 0);
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_w, buf[i].width);
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_h, buf[i].height);

    // drmModePageFlip(fd, crtc_id, buf[0].fb_id,
    // 		DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
    // Commit and check errors
    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
    if (ret < 0)
    {
        fprintf(stderr, "Plane commit failed: %s\n", strerror(-ret));
    }
    drmModeAtomicFree(req);

    sleep(3);
}

static void sigint_handler(int arg)
{
    terminate = 1;
}

int main(int argc, char **argv)
{
    int fd;
    struct modeset_output *out;
    drmModeConnector *conn;
    drmModeRes *res;
    drmModePlaneRes *plane_res;
    drmModeObjectProperties *props;
    drmModeAtomicReq *req;
    uint32_t conn_id;
    uint32_t crtc_id;

    uint32_t blob_id;
    uint32_t property_crtc_id;
    uint32_t property_mode_id;
    uint32_t property_active;
    drmEventContext ev = {};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = modeset_page_flip_handler;

    signal(SIGINT, sigint_handler);

    // fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    /* open the DRM device */
    int ret = modeset_open(&fd, "/dev/dri/card1");
    res = drmModeGetResources(fd);

    /* iterate all connectors */
    for (int i = 0; i < res->count_connectors; ++i)
    {
        /* get information for each connector */
        conn = drmModeGetConnector(fd, res->connectors[i]);
        if (!conn)
        {
            fprintf(stderr, "cannot retrieve DRM connector %u:%u (%d): %m\n",
                    i, res->connectors[i], errno);
            continue;
        }

        /* create an output structure and free connector data */
        out = modeset_output_create(fd, res, conn);
        drmModeFreeConnector(conn);
        if (!out)
            continue;

        /* link output into global list */
        out->next = output_list;
        output_list = out;
    }
    if (!output_list)
    {
        fprintf(stderr, "couldn't create any outputs\n");
        return -1;
    }

    /* free resources again */
    drmModeFreeResources(res);

    crtc_id = output_list->crtc.id;

    conn_id = output_list->connector.id;
    plane_id = output_list->plane.id;

    conn = drmModeGetConnector(fd, conn_id);
    buf[0].width = conn->modes[0].hdisplay;
    buf[0].height = conn->modes[0].vdisplay;
    buf[1].width = conn->modes[0].hdisplay;
    buf[1].height = conn->modes[0].vdisplay;

    modeset_create_fb(fd, &buf[0], 0xff0000);
    modeset_create_fb(fd, &buf[1], 0x0000ff);

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    props = drmModeObjectGetProperties(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR);
    property_crtc_id = get_property_id(fd, props, "CRTC_ID");
    drmModeFreeObjectProperties(props);

    props = drmModeObjectGetProperties(fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    property_active = get_property_id(fd, props, "ACTIVE");
    property_mode_id = get_property_id(fd, props, "MODE_ID");
    drmModeFreeObjectProperties(props);

    drmModeCreatePropertyBlob(fd, &conn->modes[0],
                              sizeof(conn->modes[0]), &blob_id);

    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, crtc_id, property_active, 1);
    drmModeAtomicAddProperty(req, crtc_id, property_mode_id, blob_id);
    drmModeAtomicAddProperty(req, conn_id, property_crtc_id, crtc_id);
    drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);

    printf("drmModeAtomicCommit SetCrtc\n");
    // getchar();

    printf("drmModeSetPlane\n");
    // drmModeSetPlane(fd, plane_id, crtc_id, buf.fb_id, 0,
    // 		50, 50, 320, 320,
    // 		0, 0, 320 << 16, 320 << 16);
    // Get plane properties
    props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    prop_fb_id = get_property_id(fd, props, "FB_ID");
    prop_crtc_id = get_property_id(fd, props, "CRTC_ID");
    prop_src_x = get_property_id(fd, props, "SRC_X");
    prop_src_y = get_property_id(fd, props, "SRC_Y");
    prop_src_w = get_property_id(fd, props, "SRC_W");
    prop_src_h = get_property_id(fd, props, "SRC_H");
    prop_crtc_x = get_property_id(fd, props, "CRTC_X");
    prop_crtc_y = get_property_id(fd, props, "CRTC_Y");
    prop_crtc_w = get_property_id(fd, props, "CRTC_W");
    prop_crtc_h = get_property_id(fd, props, "CRTC_H");
    drmModeFreeObjectProperties(props);

    // Set up atomic request for the plane
    // req = drmModeAtomicAlloc();
    // drmModeAtomicAddProperty(req, plane_id, prop_crtc_id, crtc_id);
    // drmModeAtomicAddProperty(req, plane_id, prop_fb_id, buf.fb_id);
    // drmModeAtomicAddProperty(req, plane_id, prop_src_x, 0);
    // drmModeAtomicAddProperty(req, plane_id, prop_src_y, 0);
    // drmModeAtomicAddProperty(req, plane_id, prop_src_w, 320 << 16); // 16.16 fixed point
    // drmModeAtomicAddProperty(req, plane_id, prop_src_h, 320 << 16);
    // drmModeAtomicAddProperty(req, plane_id, prop_crtc_x, 50);
    // drmModeAtomicAddProperty(req, plane_id, prop_crtc_y, 50);
    // drmModeAtomicAddProperty(req, plane_id, prop_crtc_w, 320);
    // drmModeAtomicAddProperty(req, plane_id, prop_crtc_h, 320);

    req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_id, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, prop_fb_id, buf[0].fb_id);
    drmModeAtomicAddProperty(req, plane_id, prop_src_x, 0);
    drmModeAtomicAddProperty(req, plane_id, prop_src_y, 0);
    drmModeAtomicAddProperty(req, plane_id, prop_src_w, buf[0].width << 16); // Full buffer
    drmModeAtomicAddProperty(req, plane_id, prop_src_h, buf[0].height << 16);
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_x, 0); // Start at top-left
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_y, 0);
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_w, buf[0].width);
    drmModeAtomicAddProperty(req, plane_id, prop_crtc_h, buf[0].height);

    // drmModePageFlip(fd, crtc_id, buf[0].fb_id,
    // 		DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
    // Commit and check errors
    ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
    if (ret < 0)
    {
        fprintf(stderr, "Plane commit failed: %s\n", strerror(-ret));
    }
    drmModeAtomicFree(req);

    while (!terminate)
    {
        drmHandleEvent(fd, &ev);
    }

    getchar();

    modeset_destroy_fb(fd, &buf[0]);
    modeset_destroy_fb(fd, &buf[1]);

    drmModeFreeConnector(conn);
    drmModeFreePlaneResources(plane_res);
    drmModeFreeResources(res);

    close(fd);

    return 0;
}