// probe_v4l2.c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define REQ_BUF_COUNT 4

struct buffer {
    void *start;
    size_t length;
};

static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static const char *fcc(__u32 fmt) {
    static char s[5];
    s[0] = fmt & 0xff;
    s[1] = (fmt >> 8) & 0xff;
    s[2] = (fmt >> 16) & 0xff;
    s[3] = (fmt >> 24) & 0xff;
    s[4] = 0;
    return s;
}

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/video0vicam";
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }

    // 1) VIDIOC_QUERYCAP
    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); return 2; }
    printf("driver=%s card=%s bus=%s\n", cap.driver, cap.card, cap.bus_info);
    printf("caps=0x%08x device_caps=0x%08x\n", cap.capabilities, cap.device_caps);

    // 2) ENUM_FMT
    printf("=== ENUM_FMT (VIDEO_CAPTURE) ===\n");
    struct v4l2_fmtdesc fmtd;
    memset(&fmtd, 0, sizeof(fmtd));
    fmtd.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (fmtd.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fmtd) == 0; fmtd.index++) {
        printf("fmt[%u]: %s (%s) flags=0x%x\n", fmtd.index, fcc(fmtd.pixelformat), fmtd.description, fmtd.flags);
    }

    // 3) G_FMT
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("VIDIOC_G_FMT");
    } else {
        printf("G_FMT: %ux%u pixfmt=%s field=%u bytesperline=%u sizeimage=%u\n",
               fmt.fmt.pix.width, fmt.fmt.pix.height, fcc(fmt.fmt.pix.pixelformat),
               fmt.fmt.pix.field, fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
    }

    // TRY_FMT fallback to NV12 if needed
    if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0) {
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = 1280;
        fmt.fmt.pix.height = 720;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) perror("VIDIOC_S_FMT");
        else printf("S_FMT: %ux%u pixfmt=%s sizeimage=%u\n",
                    fmt.fmt.pix.width, fmt.fmt.pix.height, fcc(fmt.fmt.pix.pixelformat), fmt.fmt.pix.sizeimage);
    }

    // REQBUFS
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = REQ_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("VIDIOC_REQBUFS"); return 3; }
    if (req.count < 2) { fprintf(stderr, "insufficient buffers: %u\n", req.count); return 4; }

    struct buffer *bufs = calloc(req.count, sizeof(*bufs));
    if (!bufs) return 5;

    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("VIDIOC_QUERYBUF"); return 6; }

        bufs[i].length = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED) { perror("mmap"); return 7; }

        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); return 8; }
    }

    // 4) STREAMON
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("VIDIOC_STREAMON"); return 9; }
    printf("STREAMON ok\n");

    // wait frame
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {2, 0};
    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) { perror("select"); return 10; }

    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("VIDIOC_DQBUF"); return 11; }

    // 5) dump one frame
    const char *out = "/data/local/tmp/frame.bin";
    int ofd = open(out, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (ofd >= 0) {
        write(ofd, bufs[b.index].start, b.bytesused);
        close(ofd);
        printf("frame saved: %s bytes=%u idx=%u\n", out, b.bytesused, b.index);
    } else {
        perror("open frame file");
    }

    xioctl(fd, VIDIOC_QBUF, &b);
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    close(fd);
    return 0;
}
