#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>

#include "mlib.h"

#define BUF_SIZE (1 << 8)

/* TODO cursor cache */
static struct {
    uint32_t last_x;
    uint32_t last_y;
    uint32_t w;
    uint32_t h;
    uint8_t *bits;
} cursor_cache;

int copy_ximage_rows_to_buffer(MBuffer *buf, XImage *ximg,
         uint32_t row_start, uint32_t row_end) {
    /* sanity checks */
    // if (buf->width < ximg->width ||
    //     buf->height < ximg->height) {
    //     fprintf(stderr, "output (%dx%d) insufficient for input (%dx%d)\n",
    //          buf->width, buf->height, ximg->width, ximg->height);
    //     return -1;
    // }

    /* TODO ximg->xoffset? */
    uint32_t buf_bytes_per_line = buf->stride * 4;
    uint32_t ximg_bytes_per_pixel = ximg->bits_per_pixel / 8;
    uint32_t y;
    // printf("[DEBUG] bytes pp = %d\n", bytes_per_pixel);
    // printf("[DEBUG] copying over XImage to Android buffer...\n");

    /* row-by-row copy to adjust for differing strides */
    uint32_t *buf_row, *ximg_row;
    for (y = row_start; y < row_end; ++y) {
        buf_row = buf->bits + (y * buf_bytes_per_line);
        ximg_row = ximg->data + (y * ximg->bytes_per_line);

        /*
         * we don't want to copy any extra XImage row padding
         * so we just copy up to image width instead of bytes_per_line
         */
        memcpy(buf_row, ximg_row, ximg->width * ximg_bytes_per_pixel);
    }

    return 0;
}

int copy_ximage_to_buffer(MBuffer *buf, XImage *ximg) {
    return copy_ximage_rows_to_buffer(buf, ximg, 0, ximg->height);
}

int copy_xcursor_to_buffer(MDisplay *mdpy, MBuffer *buf, XFixesCursorImage *cursor) {
    //printf("[DEBUG] painting cursor...\n");
/*    for (y = cursor->y; y < cursor->y + cursor->height; ++y) {
        memcpy(buf + (y * 1152 * 4) + (cursor->x * 4),
         cursor_buf + ((y - cursor->y) * cursor->width * 4),
         cursor->width * 4);
    }
*/

    // fprintf(stderr, "[DEBUG] cursor dims = %d x %d\n",
    //      cursor->width, cursor->height);

    int err;
    err = MLockBuffer(mdpy, buf);
    if (err < 0) {
        fprintf(stderr, "MLockBuffer failed!\n");
        return -1;
    }

    uint32_t cur_x, cur_y;  /* cursor relative coords */
    uint32_t x, y;          /* root window coords */
    for (cur_y = 0; cur_y < cursor->height; ++cur_y) {
        for (cur_x = 0; cur_x < cursor->width; ++cur_x) {
            x = cur_x;
            y = cur_y;

            /* bounds check! */
            if (y >= buf->height || x >= buf->width) {
                break;
            }

            uint8_t *pixel = cursor_cache.bits +
                4 * (cur_y * cursor->width + cur_x);

            // printf("[DEBUG] (%d, %d) -> (%d, %d, %d, %d)\n",
            //     cursor->x + x, cursor->y + y, pixel[0], pixel[1], pixel[2], pixel[3]);

            /* copy only if opaque pixel */
            if (pixel[3] == 255) {
                uint32_t *buf_pixel = buf->bits + (y * buf->stride + x) * 4;
                memcpy(buf_pixel, pixel, 4);
            }
        }
    }

    err = MUnlockBuffer(mdpy, buf);
    if (err < 0) {
        fprintf(stderr, "MUnlockBuffer failed!\n");
        return -1;
    }

    return 0;
}

int Xrender(Display *dpy, MBuffer *buf, XImage *ximg) {
    /* grab the current root window framebuffer */
    //printf("[DEBUG] grabbing root window...\n");
    Status status;
    status = XShmGetImage(dpy,
        DefaultRootWindow(dpy),
        ximg,
        0, 0,
        AllPlanes);
    if(!status) {
        fprintf(stderr, "error calling XShmGetImage\n");
    }

    copy_ximage_to_buffer(buf, ximg);

    return 0;
 }

int run(Display *dpy, MDisplay *mdpy, MBuffer *buf, XImage *ximg) {
    int err;

    err = MLockBuffer(mdpy, buf);
    if (err < 0) {
        fprintf(stderr, "MLockBuffer failed!\n");
        return -1;
    }

    // printf("[DEBUG] buf.width = %d\n", buf.width);
    // printf("[DEBUG] buf.height = %d\n", buf.height);
    // printf("[DEBUG] buf.stride = %d\n", buf.stride);
    // printf("[DEBUG] buf_fd = %d\n", buf_fd);

    Xrender(dpy, buf, ximg);
    // fillBufferRGBA8888((uint8_t *)buf.bits, 0, 0, buf.width, buf.height, r, g, b);

    err = MUnlockBuffer(mdpy, buf);
    if (err < 0) {
        fprintf(stderr, "MUnlockBuffer failed!\n");
        return -1;
    }

    return 0;
}

int main(void) {
    Display *dpy;
    MDisplay mdpy;
    int err = 0;

    /* TODO Ctrl-C handler to cleanup shm */

    /* connect to the X server using the DISPLAY environment variable */
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "error calling XOpenDisplay\n");
        return -1;
    }

    //
    // check for necessary eXtensions
    //
    int xfixes_event_base, error;
    if (!XFixesQueryExtension(dpy, &xfixes_event_base, &error)) {
        fprintf(stderr, "Xfixes extension unavailable!\n");
        return -1;
    }

    if (!XShmQueryExtension(dpy)) {
        fprintf(stderr, "XShm extension unavailable!\n");
        return -1;
    }

    int xdamage_event_base;
    if (!XDamageQueryExtension(dpy, &xdamage_event_base, &error)) {
        fprintf(stderr, "XDamage extension unavailable!\n");
        return -1;
    }

    /* connect to maru display server */
    if (MOpenDisplay(&mdpy) < 0) {
        fprintf(stderr, "error calling MOpenDisplay\n");
        return -1;
    }

    //
    // Create necessary buffers
    //

    int screen = DefaultScreen(dpy);

    /* root window buffer */
    MBuffer root;
    root.width = XDisplayWidth(dpy, screen);
    root.height = XDisplayHeight(dpy, screen);
    if (MCreateBuffer(&mdpy, &root) < 0) {
        printf("Error calling MCreateBuffer\n");
    }
    printf("[DEBUG] root.__id = %d\n", root.__id);

    /* cursor buffer */
    /* TODO free xcursor? */
    XFixesCursorImage *xcursor = XFixesGetCursorImage(dpy);
    if (cursor_cache.bits == NULL) {
        cursor_cache.bits = (uint8_t *)xcursor->pixels;
    }
    cursor_cache.last_x = xcursor->x;
    cursor_cache.last_y = xcursor->y;
    cursor_cache.w = xcursor->width;
    cursor_cache.h = xcursor->height;

    MBuffer cursor;
    cursor.width = xcursor->width;
    cursor.height = xcursor->height;
    if (MCreateBuffer(&mdpy, &cursor) < 0) {
        printf("Error calling MCreateBuffer\n");
    }
    printf("[DEBUG] cursor.__id = %d\n", cursor.__id);

    /* render cursor sprite */
    if (copy_xcursor_to_buffer(&mdpy, &cursor, xcursor) < 0) {
        fprintf(stderr, "failed to render cursor sprite\n");
    }

    //
    // create shared memory XImage structure
    //
    XShmSegmentInfo shminfo;
    XImage *ximg = XShmCreateImage(dpy,
                        DefaultVisual(dpy, screen),
                        DefaultDepth(dpy, screen),
                        ZPixmap,
                        NULL,
                        &shminfo,
                        XDisplayWidth(dpy, screen),
                        XDisplayHeight(dpy, screen));
    if (ximg == NULL) {
        fprintf(stderr, "error creating XShm Ximage\n");
        return -1;
    }

    //
    // create a shared memory segment to store actual image data
    //
    shminfo.shmid = shmget(IPC_PRIVATE,
             ximg->bytes_per_line * ximg->height, IPC_CREAT|0777);
    if (shminfo.shmid < 0) {
        fprintf(stderr, "error creating shm segment: %s\n", strerror(errno));
        return -1;
    }

    shminfo.shmaddr = ximg->data = shmat(shminfo.shmid, NULL, 0);
    if (shminfo.shmaddr < 0) {
        fprintf(stderr, "error attaching shm segment: %s\n", strerror(errno));
        err = -1;
        goto cleanup_shm;
    }

    shminfo.readOnly = False;

    //
    // inform server of shm
    //
    if (!XShmAttach(dpy, &shminfo)) {
        fprintf(stderr, "error calling XShmAttach\n");
        err = -1;
        goto cleanup_X;
    }

    //
    // register for X events
    //
    XFixesSelectCursorInput(dpy, DefaultRootWindow(dpy),
         XFixesDisplayCursorNotifyMask);
    XSelectInput(dpy, DefaultRootWindow(dpy), PointerMotionMask);

    XDamageCreate(dpy, DefaultRootWindow(dpy), XDamageReportRawRectangles);

    /* event loop */
    XEvent ev;
    Bool repaint;
    for (;;) {
        /* TODO check that we don't queue up damage... */
        repaint = XCheckTypedEvent(dpy, xdamage_event_base + XDamageNotify, &ev);

        if (repaint) {
            run(dpy, &mdpy, &root, ximg);
        }

        /* TODO switch to XQueryCursor */
        xcursor = XFixesGetCursorImage(dpy);
        // fprintf(stderr, "[DEBUG] cursor pos = (%d, %d)\n",
        //      xcursor->x, xcursor->y);
        if (xcursor->x != cursor_cache.last_x ||
            xcursor->y != cursor_cache.last_y) {
            /* adjust so that hotspot is top-left */
            int32_t xpos = xcursor->x - xcursor->xhot;
            int32_t ypos = xcursor->y - xcursor->yhot;

            /* enforce lower bound or surfaceflinger freaks out */
            if (xpos < 0) {
                xpos = 0;
            }
            if (ypos < 0) {
                ypos = 0;
            }

            if (MUpdateBuffer(&mdpy, &cursor, xpos, ypos) < 0) {
                fprintf(stderr, "error calling MUpdateBuffer\n");
            }

            cursor_cache.last_x = xcursor->x;
            cursor_cache.last_y = xcursor->y;
        }

        XFree(xcursor);

        // XNextEvent(dpy, &ev);
        // if (ev.type == xdamage_event_base + XDamageNotify) {
        //     fprintf(stderr, "XDamageNotify!\n");
        //     run(dpy, &mdpy, ximg, 1);
        // } else if (ev.type == MotionNotify) {
        //     fprintf(stderr, "MotionNotify!\n");
        //     run(dpy, &mdpy, ximg);
        // } else if (ev.type == xfixes_event_base + XFixesDisplayCursorNotify) {
        //     fprintf(stderr, "XFixesDisplayCursorNotify!\n");
        // }
        // switch (ev.type) {
        // // case XFixesDisplayCursorNotify:
        // //     fprintf(stderr, "XFixesDisplayCursorNotify event!\n");
        // //     // cev = (XFixesCursorNotifyEvent)ev;
        // //     // fprintf(stderr, "cursor-serial: %d\n", cev.cursor_serial);
        // //     break;

        // case MotionNotify:
        //     fprintf(stderr, "pointer coords = (%d, %d)\n",
        //          ev.xmotion.x_root, ev.xmotion.y_root);
        //     break;

        // case xdamage_event_base + XDamageNotify:
        //     fprintf(stderr, "XDamageNotify!\n");
        //     run(dpy, &mdpy, ximg);
        //     break;

        // default:
        //     fprintf(stderr, "received unexpected event: %d\n", ev.type);
        //     break;
        // }
    }

cleanup_X:

    if (!XShmDetach(dpy, &shminfo)) {
        fprintf(stderr, "error detaching shm from X server\n");
    }

    XDestroyImage(ximg);

cleanup_shm:

    if (shmdt(shminfo.shmaddr) < 0) {
        fprintf(stderr, "error detaching shm: %s\n", strerror(errno));
    }

    if (shmctl(shminfo.shmid, IPC_RMID, 0) < 0) {
        fprintf(stderr, "error destroying shm: %s\n", strerror(errno));
    }

    return err;
}