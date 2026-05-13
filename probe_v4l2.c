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
#define MAX_PLANES 8

struct buffer {
    void *start[MAX_PLANES];
    size_t length[MAX_PLANES];
    int planes;
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

static void enum_formats(int fd, enum v4l2_buf_type type) {
    struct v4l2_fmtdesc fmtd;
    memset(&fmtd, 0, sizeof(fmtd));
    fmtd.type = type;
    printf("=== ENUM_FMT type=%d ===\n", type);
    for (fmtd.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fmtd) == 0; fmtd.index++) {
        printf("fmt[%u]: %s (%s) flags=0x%x\n", fmtd.index, fcc(fmtd.pixelformat), fmtd.description, fmtd.flags);
    }
}

static int try_stream_capture(int fd, enum v4l2_buf_type type, int mplane, const char *out) {
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = type;

    if (xioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("VIDIOC_G_FMT");
    } else {
        if (!mplane) {
            printf("G_FMT CAP: %ux%u pixfmt=%s bpl=%u size=%u\n",
                   fmt.fmt.pix.width, fmt.fmt.pix.height, fcc(fmt.fmt.pix.pixelformat),
                   fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
        } else {
            printf("G_FMT MPLANE: %ux%u pixfmt=%s planes=%u\n",
                   fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, fcc(fmt.fmt.pix_mp.pixelformat),
                   fmt.fmt.pix_mp.num_planes);
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = REQ_BUF_COUNT;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }
    if (req.count < 2) {
        fprintf(stderr, "insufficient buffers: %u\n", req.count);
        return -1;
    }

    struct buffer *bufs = calloc(req.count, sizeof(*bufs));
    if (!bufs) return -1;

    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane planes[MAX_PLANES];
        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (mplane) {
            b.m.planes = planes;
            b.length = MAX_PLANES;
        }

        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) {
            perror("VIDIOC_QUERYBUF");
            free(bufs);
            return -1;
        }

        if (!mplane) {
            bufs[i].planes = 1;
            bufs[i].length[0] = b.length;
            bufs[i].start[0] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
            if (bufs[i].start[0] == MAP_FAILED) { perror("mmap"); free(bufs); return -1; }
        } else {
            bufs[i].planes = b.length;
            if (bufs[i].planes > MAX_PLANES) bufs[i].planes = MAX_PLANES;
            for (int p = 0; p < bufs[i].planes; p++) {
                bufs[i].length[p] = planes[p].length;
                bufs[i].start[p] = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[p].m.mem_offset);
                if (bufs[i].start[p] == MAP_FAILED) { perror("mmap plane"); free(bufs); return -1; }
            }
        }

        memset(planes, 0, sizeof(planes));
        memset(&b, 0, sizeof(b));
        b.type = type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (mplane) {
            b.m.planes = planes;
            b.length = MAX_PLANES;
        }
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) {
            perror("VIDIOC_QBUF");
            free(bufs);
            return -1;
        }
    }

    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        free(bufs);
        return -1;
    }
    printf("STREAMON ok type=%d\n", type);

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {2, 0};
    int r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) {
        perror("select");
        xioctl(fd, VIDIOC_STREAMOFF, &type);
        free(bufs);
        return -1;
    }

    struct v4l2_buffer b;
    struct v4l2_plane planes[MAX_PLANES];
    memset(&b, 0, sizeof(b));
    memset(planes, 0, sizeof(planes));
    b.type = type;
    b.memory = V4L2_MEMORY_MMAP;
    if (mplane) {
        b.m.planes = planes;
        b.length = MAX_PLANES;
    }

    if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
        perror("VIDIOC_DQBUF");
        xioctl(fd, VIDIOC_STREAMOFF, &type);
        free(bufs);
        return -1;
    }

    int ofd = open(out, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (ofd >= 0) {
        if (!mplane) {
            write(ofd, bufs[b.index].start[0], b.bytesused);
            printf("frame saved: %s bytes=%u\n", out, b.bytesused);
        } else {
            unsigned total = 0;
            for (unsigned p = 0; p < b.length && p < MAX_PLANES; p++) {
                unsigned used = planes[p].bytesused;
                if (used > bufs[b.index].length[p]) used = bufs[b.index].length[p];
                write(ofd, bufs[b.index].start[p], used);
                total += used;
            }
            printf("frame saved: %s total_bytes=%u planes=%u\n", out, total, b.length);
        }
        close(ofd);
    } else {
        perror("open out");
    }

    xioctl(fd, VIDIOC_QBUF, &b);
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    free(bufs);
    return 0;
}

int main(int argc, char **argv) {
    const char *dev = (argc > 1) ? argv[1] : "/dev/video0vicam";
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { perror("open"); return 1; }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("VIDIOC_QUERYCAP"); return 2; }
    printf("driver=%s card=%s bus=%s\n", cap.driver, cap.card, cap.bus_info);
    printf("caps=0x%08x device_caps=0x%08x\n", cap.capabilities, cap.device_caps);

    enum_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    enum_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

    printf("=== TRY CAPTURE ===\n");
    if (try_stream_capture(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0, "/data/local/tmp/frame_cap.bin") == 0) {
        close(fd);
        return 0;
    }

    printf("=== TRY MPLANE ===\n");
    if (try_stream_capture(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, 1, "/data/local/tmp/frame_mplane.bin") == 0) {
        close(fd);
        return 0;
    }

    close(fd);
    return 3;
}
