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

struct buffer_object {
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

static int modeset_create_fb(int fd, struct buffer_object *bo,uint32_t color)
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
	uint32_t handles[4] = { bo->handle };
	uint32_t pitches[4] = { bo->pitch };
	uint32_t offsets[4] = { 0 };
	uint32_t format = DRM_FORMAT_XRGB8888; // Explicit format

	drmModeAddFB2(fd, bo->width, bo->height, format, handles, pitches, offsets, &bo->fb_id, 0);

	map.handle = create.handle;
	drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);

	bo->vaddr = mmap(0, create.size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, map.offset);

	//memset(bo->vaddr, 0xf0, bo->size);
	uint32_t *pixel = (uint32_t*)bo->vaddr;
	for (size_t i = 0; i < (bo->size / 4); i++) {
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

	for (i = 0; i < props->count_props; i++) {
		property = drmModeGetProperty(fd, props->props[i]);
		if (!strcmp(property->name, name))
			id = property->prop_id;
		drmModeFreeProperty(property);

		if (id)
			break;
	}

	return id;
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


//handle vblank(vsync) event
static void modeset_page_flip_handler(int fd, uint32_t frame,
				    uint32_t sec, uint32_t usec,
				    void *data)
{
	static int i = 0;
	uint32_t crtc_id = *(uint32_t *)data;

	i ^= 1;
	printf("fd %d crtc_id %d buf[i].fb_id %d data %llx \n",fd,crtc_id,buf[i].fb_id,data);
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
int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET| DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
if (ret < 0) {
    fprintf(stderr, "Plane commit failed: %s\n", strerror(-ret));
}
drmModeAtomicFree(req);


	sleep(10);
}


static void sigint_handler(int arg)
{
	terminate = 1;
}

int main(int argc, char **argv)
{
	int fd;
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

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);

	res = drmModeGetResources(fd);
	crtc_id = res->crtcs[0];
	conn_id = res->connectors[0];

	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	plane_res = drmModeGetPlaneResources(fd);
	plane_id = plane_res->planes[0]; 


	conn = drmModeGetConnector(fd, conn_id);
	buf[0].width = conn->modes[0].hdisplay;
	buf[0].height = conn->modes[0].vdisplay;
	buf[1].width = conn->modes[0].hdisplay;
	buf[1].height = conn->modes[0].vdisplay;


	modeset_create_fb(fd, &buf[0],0xff0000);
	modeset_create_fb(fd, &buf[1], 0x0000ff);

	drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

	props = drmModeObjectGetProperties(fd, conn_id,	DRM_MODE_OBJECT_CONNECTOR);
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
	drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);

	printf("drmModeAtomicCommit SetCrtc\n");
	getchar();

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
int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET| DRM_MODE_PAGE_FLIP_EVENT, &crtc_id);
if (ret < 0) {
    fprintf(stderr, "Plane commit failed: %s\n", strerror(-ret));
}
drmModeAtomicFree(req);



	while (!terminate) {
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
