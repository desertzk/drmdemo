/*
 * Author: Leon.He
 * e-mail: 343005384@qq.com
 */

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
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buffer_object {
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t handle;
	uint32_t size;
	uint8_t *vaddr;
	uint32_t fb_id;
};

struct buffer_object buf;

static int modeset_create_fb(int fd, struct buffer_object *bo)
{
	struct drm_mode_create_dumb create = {};
 	struct drm_mode_map_dumb map = {};

	//create a dumb-buffer, the pixel format is XRGB888
	create.width = bo->width;
	create.height = bo->height;
	create.bpp = 32;  //bits per pixel
	drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);//DRM_IOCTL_MODE_CREATE_DUMB ioctl to allocate a simple memory buffer for the framebuffer.

	//bind the dumb-buffer to  an FB object
	bo->pitch = create.pitch;
	bo->size = create.size;
	bo->handle = create.handle;
	//associate the buffer with a framebuffer ID.
	drmModeAddFB(fd, bo->width, bo->height, 24, 32, bo->pitch,
			   bo->handle, &bo->fb_id);

	//map the dumb-buffer to userspace
	map.handle = create.handle;
	drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);

	bo->vaddr = mmap(0, create.size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, map.offset);
	//set 0xff white color
	memset(bo->vaddr, 0xff, bo->size);

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

int main(int argc, char **argv)
{
	int fd;
	drmModeConnector *conn;
	drmModeRes *res;
	uint32_t conn_id;
	uint32_t crtc_id;
    struct buffer_object *bo=&buf;
    uint32_t color=0x000000;

    // Open the DRM device
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("Failed to open /dev/dri/card0");
        return EXIT_FAILURE;
    }

    // Get DRM resources
    res = drmModeGetResources(fd);
    if (!res) {
        perror("drmModeGetResources failed");
        close(fd);
        return EXIT_FAILURE;
    }

    // Ensure at least one CRTC is available
    if (res->count_crtcs == 0) {
        fprintf(stderr, "No CRTCs found\n");
        drmModeFreeResources(res);
        close(fd);
        return EXIT_FAILURE;
    }
    crtc_id = res->crtcs[0];

    // Ensure at least one connector is available
    if (res->count_connectors == 0) {
        fprintf(stderr, "No connectors found\n");
        drmModeFreeResources(res);
        close(fd);
        return EXIT_FAILURE;
    }
    conn_id = res->connectors[0];

    // Get connector information
    conn = drmModeGetConnector(fd, conn_id);
    if (!conn) {
        perror("drmModeGetConnector failed");
        drmModeFreeResources(res);
        close(fd);
        return EXIT_FAILURE;
    }

    // Ensure the connector has at least one mode
    if (conn->count_modes == 0) {
        fprintf(stderr, "No valid modes found for connector\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(fd);
        return EXIT_FAILURE;
    }

    buf.width = conn->modes[0].hdisplay;
    buf.height = conn->modes[0].vdisplay;

    // Create framebuffer
    if (modeset_create_fb(fd, &buf) != 0) {
        fprintf(stderr, "Failed to create framebuffer\n");
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(fd);
        return EXIT_FAILURE;
    }

    // Set CRTC
    if (drmModeSetCrtc(fd, crtc_id, buf.fb_id, 0, 0, &conn_id, 1, &conn->modes[0]) != 0) {
        perror("drmModeSetCrtc failed");
        modeset_destroy_fb(fd, &buf);
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        close(fd);
        return EXIT_FAILURE;
    }

    printf("Press Enter to exit...\n");
    getchar();

    for (int i = 0; i < (bo->size / 4); i++){
        ++color;
        bo->vaddr[i] = color;
        if(i%10==0)
        {
            printf("color %d \n",color);
             // Set CRTC
            if (drmModeSetCrtc(fd, crtc_id, buf.fb_id, 0, 0, &conn_id, 1, &conn->modes[0]) != 0) {
                perror("drmModeSetCrtc failed");
                modeset_destroy_fb(fd, &buf);
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                close(fd);
                return EXIT_FAILURE;
            }
            //sleep(1);
        }
        
    }
        

    printf("Press Enter to exit...\n");
    getchar();
    // Clean up resources
    modeset_destroy_fb(fd, &buf);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(fd);

    return EXIT_SUCCESS;

}
