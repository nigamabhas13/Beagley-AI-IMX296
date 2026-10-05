// SPDX-License-Identifier: GPL-2.0
/*
 * imx296-capture - bring up the CSI1 pipeline and grab frames from the IMX296
 * on a BeagleY-AI.  Self-contained: v4l-utils is not installed on the board.
 *
 * Does, in the order media-ctl + v4l2-ctl would:
 *   1. enumerate the media graph and print it
 *   2. enable every mutable link
 *   3. set a 10-bit mono format on every subdev pad in the path
 *   4. size the capture video node to match the sensor
 *   5. stream, dequeue frames, print statistics from the first one
 *
 * Streaming mode (--stdout) demosaics 10-bit BGGR into YUYV, crops to the
 * stream geometry (1280x720) and writes it to stdout for gstreamer:
 *
 *   ./imx296-capture --stdout --fps 30 --awb /dev/media1 | \
 *     gst-launch-1.0 -q fdsrc fd=0 ! \
 *     rawvideoparse format=yuy2 width=1280 height=720 framerate=30/1 ! \
 *     v4l2h264enc ! h264parse config-interval=1 ! \
 *     rtph264pay pt=96 config-interval=1 mtu=1400 ! \
 *     udpsink host=192.168.1.106 port=5000 sync=false async=false
 *
 * The Wave5 encoder takes YUYV directly, so no videoconvert or videoscale
 * is needed - keeping those two software elements out is what lets the
 * encoder hold 30 fps.
 *
 * Options:
 *   --fps N        slow the sensor to N fps via VBLANK (default: sensor rate)
 *   --exposure N   manual exposure in lines (driver default 1104); pins the AE
 *   --gain N       analogue gain, 0-480 in 0.1 dB steps; pins the AE's gain
 *
 * With neither flag the tool auto-exposes: exposure first, up to just short
 * of VMAX, then analogue gain.  Passing --gain pins the gain and leaves the
 * AE working with exposure alone.
 *   --setup-only   configure the graph and exit without streaming
 *
 * Entities are classified by their /dev node name (v4l-subdevN / videoN),
 * not by media_entity_desc.type: with the function-based entity types a
 * subdev's type is e.g. MEDIA_ENT_F_CAM_SENSOR, never MEDIA_ENT_T_V4L2_SUBDEV.
 *
 * Build:  gcc -O2 -o imx296-capture imx296-capture.c
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXENT	256
#define MAXBUF	4

#define SENSOR_W	1456
#define SENSOR_H	1088
#define DEMO_W		1456	/* demosaic row cache width */

/*
 * stdout mode crops to the stream geometry, 1280x720.  H.264 codes 16x16
 * macroblocks and the Wave5 encoder refuses anything else: 1456x818 (the
 * old crop) fails caps negotiation with not-negotiated.  1280 and 720 are
 * both multiples of 16, and the encoder takes YUYV directly, so emitting
 * exactly this size lets the pipeline run without videoconvert or
 * videoscale - which is what keeps the encoder fed at 30 fps.  The 88
 * column side crop is even, so the bayer phase is preserved.
 */
#define STREAM_W	1280
#define STREAM_H	720
#define CROP_X0		((SENSOR_W - STREAM_W) / 2)	/* 88 */
#define CROP_Y0		((SENSOR_H - STREAM_H) / 2)	/* 184 */

struct ent {
	struct media_entity_desc d;
	char name[64];
	char dev[32];		/* "video2", "v4l-subdev3", ... or "" */
};

struct buf {
	void *start;
	size_t length;
	int fd;			/* dmabuf fd, or -1 for the mmap fallback */
};

/*
 * /dev/dma_heap/linux,cma allocation.  The shim's DMA needs physically
 * contiguous buffers, and the CMA heap's userspace mapping is cached - unlike
 * the shim's own MMAP buffers (the dma-coherent pool), which read ~25 ms per
 * 3.17 MB pass.
 */
struct dma_heap_allocation_data {
	unsigned long long len;
	unsigned int fd;
	unsigned int fd_flags;
	unsigned long long heap_flags;
};

#define DMA_HEAP_IOC_MAGIC	'H'
#define DMA_HEAP_IOCTL_ALLOC	_IOWR(DMA_HEAP_IOC_MAGIC, 0x0, \
				      struct dma_heap_allocation_data)

static struct ent ents[MAXENT];
static int nents;

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

/* /dev node backing an entity, without the leading "/dev/" */
static int devnode(const struct media_entity_desc *e, char *out, size_t n)
{
	char path[128], line[256];
	FILE *f;

	snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent",
		 e->dev.major, e->dev.minor);
	f = fopen(path, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		char dev[128];

		if (sscanf(line, "DEVNAME=%127s", dev) == 1) {
			snprintf(out, n, "%.*s", (int)(n - 1), dev);
			fclose(f);
			return 0;
		}
	}
	fclose(f);
	return -1;
}

static int open_dev(const struct ent *e)
{
	char path[96];

	snprintf(path, sizeof(path), "/dev/%s", e->dev);
	return open(path, O_RDWR);
}

static int enumerate(int mfd)
{
	int i;

	for (i = 0; i < MAXENT; i++) {
		struct ent *e = &ents[nents];

		memset(e, 0, sizeof(*e));
		e->d.id = i | MEDIA_ENT_ID_FLAG_NEXT;
		if (xioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &e->d) < 0)
			break;
		if (e->d.name[0]) {
			memcpy(e->name, e->d.name, sizeof(e->d.name));
			e->name[sizeof(e->d.name)] = '\0';
		} else
			snprintf(e->name, sizeof(e->name), "0x%08x", e->d.id);
		if (e->d.dev.major)
			devnode(&e->d, e->dev, sizeof(e->dev));
		nents++;
	}
	return nents;
}

static void enable_links(int mfd)
{
	int i;

	for (i = 0; i < nents; i++) {
		struct media_links_enum links;
		unsigned int l;

		memset(&links, 0, sizeof(links));
		links.entity = ents[i].d.id;
		if (ents[i].d.pads)
			links.pads = calloc(ents[i].d.pads, sizeof(*links.pads));
		if (ents[i].d.links)
			links.links = calloc(ents[i].d.links, sizeof(*links.links));
		if ((ents[i].d.pads && !links.pads) ||
		    (ents[i].d.links && !links.links) ||
		    xioctl(mfd, MEDIA_IOC_ENUM_LINKS, &links) < 0) {
			free(links.pads);
			free(links.links);
			continue;
		}
		for (l = 0; l < ents[i].d.links; l++) {
			struct media_link_desc setup = links.links[l];

			if (setup.flags & MEDIA_LNK_FL_IMMUTABLE)
				continue;
			setup.flags |= MEDIA_LNK_FL_ENABLED;
			if (xioctl(mfd, MEDIA_IOC_SETUP_LINK, &setup) == 0)
				printf("  enabled link: %s -> entity %u\n",
				       ents[i].name, setup.sink.entity);
		}
		free(links.pads);
		free(links.links);
	}
}

/* true if this entity is a subdev (has a /dev/v4l-subdevN node) */
static int is_subdev(const struct ent *e)
{
	return !strncmp(e->dev, "v4l-subdev", 10);
}

static int is_video(const struct ent *e)
{
	return !strncmp(e->dev, "video", 5);
}

/*
 * Bilinear demosaic of a 10-bit BGGR frame into YUYV (BT.601).
 *
 * The sensor (IMX296LQ) delivers SBGGR10: 10-bit samples in 16-bit
 * little-endian words, with
 *   even rows: B G B G ...   odd rows:  G R G R ...
 * (a 2x2 quad is B G / G R, top-left to bottom-right).
 *
 * gstreamer's bayer2rgb only handles 8-bit bayer, so this demosaics here and
 * hands gstreamer plain YUYV.  Each missing channel is the average of the
 * 2-4 nearest neighbours of that colour; border pixels clamp to the edge.
 *
 * The v4l2 DMA buffers read far slower than normal memory (an uncached
 * mapping - one 3.17 MB pass costs ~25 ms), so the source is read exactly
 * ONCE, sequentially, into a rolling 3-row cache that all the interpolation
 * reads hit.  The naive re-reading version took 1.26 s per frame; this one
 * takes ~55 ms.
 */
static int first_pixel_debug = 1;
static unsigned long long wb_sum_r, wb_sum_g, wb_sum_b, wb_cnt;
static int bayer_order;	/* 0=BGGR 1=GBRG 2=GRBG 3=RGGB */

/* one YUYV pair from eight 8-bit RGB values: shared by both row loops */
static inline void yuv_pair(uint8_t *o, unsigned int rg, unsigned int bg,
			    unsigned int r0, unsigned int g0, unsigned int b0,
			    unsigned int r1, unsigned int g1, unsigned int b1)
{
	unsigned int ra, ga, ba, u, v, y0, y1;

	/* black level: the sensor's raw floor (~60/1023) - subtract it so
	 * shadows go to true black and the channel offsets stop tinting
	 * the dark areas, then 10-bit -> 8-bit with the WB gains. */
#define BLK 60
	r0 = (r0 > BLK ? r0 - BLK : 0) * rg >> 12;
	g0 = (g0 > BLK ? g0 - BLK : 0) >> 2;
	b0 = (b0 > BLK ? b0 - BLK : 0) * bg >> 12;
	r1 = (r1 > BLK ? r1 - BLK : 0) * rg >> 12;
	g1 = (g1 > BLK ? g1 - BLK : 0) >> 2;
	b1 = (b1 > BLK ? b1 - BLK : 0) * bg >> 12;
#undef BLK

	/* The WB gains reach 4x (rg/bg up to 4096) and g stays at unity, so
	 * a bright channel can land well past 255 here.  Y/U/V are written
	 * through (uint8_t), which wraps instead of clipping - a pixel at
	 * 300 comes out as 44 and paints the highlight cyan or magenta.
	 * Clip the RGB first; BT.601 below then can't leave 0..255. */
#define CLAMP_U8(value) ((value) > 255 ? 255 : (value))
	r0 = CLAMP_U8(r0);
	g0 = CLAMP_U8(g0);
	b0 = CLAMP_U8(b0);
	r1 = CLAMP_U8(r1);
	g1 = CLAMP_U8(g1);
	b1 = CLAMP_U8(b1);
#undef CLAMP_U8

	/* BT.601: Y per pixel, U/V shared across the pair */
	ra = (r0 + r1) >> 1;
	ga = (g0 + g1) >> 1;
	ba = (b0 + b1) >> 1;
	y0 = (77 * r0 + 150 * g0 + 29 * b0 + 128) >> 8;
	y1 = (77 * r1 + 150 * g1 + 29 * b1 + 128) >> 8;
	u = 128 + ((-43 * (int)ra - 85 * (int)ga + 128 * (int)ba) >> 8);
	v = 128 + ((128 * (int)ra - 107 * (int)ga - 21 * (int)ba) >> 8);

	o[0] = (uint8_t)y0;
	o[1] = (uint8_t)u;
	o[2] = (uint8_t)y1;
	o[3] = (uint8_t)v;

	if (first_pixel_debug) {
		first_pixel_debug = 0;
		fprintf(stderr, "dbg yuv: r0=%u g0=%u b0=%u r1=%u g1=%u b1=%u -> y0=%u u=%u y1=%u v=%u\n",
			r0, g0, b0, r1, g1, b1, y0, u, y1, v);
	}
	wb_sum_r += r0 + r1;
	wb_sum_g += g0 + g1;
	wb_sum_b += b0 + b1;
	wb_cnt += 2;
}

static void demosaic_bggr10_to_yuyv(const uint16_t *in, uint8_t *out,
				    unsigned int w, unsigned int h,
				    unsigned int rg, unsigned int bg,
				    unsigned int x0, unsigned int x1,
				    unsigned int y0, unsigned int y1)
{
	uint16_t row[3][DEMO_W];
	unsigned int x, yy, n;

	if (w > DEMO_W)
		w = DEMO_W;
	if (x1 > w || x0 >= x1) {
		x0 = 0;
		x1 = w & ~1u;
	}
	if (y1 > h)
		y1 = h;
	if (y0 >= y1 || y0 > h) {
		y0 = 0;
		y1 = h;
	}
	n = y1 - y0;
	in += y0 * w;

	/* slots: (y-1), y, (y+1).  Rows y0, y0+1 and y0-1 (or y0 at the top). */
	memcpy(row[0], in, w * 2);
	memcpy(row[1], in + w, w * 2);
	memcpy(row[2], y0 ? in - w : in, w * 2);

	for (yy = 0; yy < n; yy++) {
		const uint16_t *ru = row[(yy + 2) % 3];	/* y-1 (wraps) */
		const uint16_t *rc = row[yy % 3];	/* y */
		const uint16_t *rd = row[(yy + 1) % 3];	/* y+1 */
		uint8_t *o = out + yy * (x1 - x0) * 2;

		/* clamp the border rows to y itself */
		if (yy == 0 && y0 == 0)
			ru = rc;
		if (yy == n - 1 && y1 == h)
			rd = rc;

		/*
		 * The row parity is hoisted out of the pixel loop: an even
		 * row is B G B G ... and an odd row G R G R ..., so the
		 * neighbour patterns are fixed per row.
		 */
		{
			unsigned int xm1, xp2, c1, c2, h1, v1, d1, h2, v2, d2, n1, n2;

			if ((yy + y0) & 1) {
				/* odd row: pair = (R or G, G or B) by order */
				for (x = x0; x < x1; x += 2) {
					xm1 = x ? x - 1 : 0;
					xp2 = x + 2 < w ? x + 2 : w - 1;
					c1 = rc[x] & 0x3ff;
					c2 = rc[x + 1] & 0x3ff;
					h1 = (rc[xm1] + rc[x + 1]) >> 1;
					v1 = (ru[x] + rd[x]) >> 1;
					d1 = (ru[xm1] + ru[x + 1] + rd[xm1] +
					      rd[x + 1]) >> 2;
					n1 = (rc[xm1] + rc[x + 1] + ru[x] +
					      rd[x]) >> 2;
					n2 = (rc[x] + rc[xp2] + ru[x + 1] +
					      rd[x + 1]) >> 2;
					h2 = (rc[x] + rc[xp2]) >> 1;
					v2 = (ru[x + 1] + rd[x + 1]) >> 1;
					d2 = (ru[x] + ru[xp2] + rd[x] +
					      rd[xp2]) >> 2;

					switch (bayer_order) {
					case 0:	/* BGGR: [G R] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 h1, c1, v1, c2, n2, d2);
						break;
					case 1:	/* GBRG: [R G] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 c1, n1, d1, v2, c2, h2);
						break;
					case 2:	/* GRBG: [B G] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 d1, n1, c1, v2, c2, h2);
						break;
					default: /* RGGB: [G B] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 v1, c1, h1, d2, n2, c2);
						break;
					}
				}
			} else {
				/* even row: pair = (B or G, G or R) by order */
				for (x = x0; x < x1; x += 2) {
					xm1 = x ? x - 1 : 0;
					xp2 = x + 2 < w ? x + 2 : w - 1;
					c1 = rc[x] & 0x3ff;
					c2 = rc[x + 1] & 0x3ff;
					h1 = (rc[xm1] + rc[x + 1]) >> 1;
					v1 = (ru[x] + rd[x]) >> 1;
					d1 = (ru[xm1] + ru[x + 1] + rd[xm1] +
					      rd[x + 1]) >> 2;
					n1 = (rc[xm1] + rc[x + 1] + ru[x] +
					      rd[x]) >> 2;
					n2 = (rc[x] + rc[xp2] + ru[x + 1] +
					      rd[x + 1]) >> 2;
					h2 = (rc[x] + rc[xp2]) >> 1;
					v2 = (ru[x + 1] + rd[x + 1]) >> 1;
					d2 = (ru[x] + ru[xp2] + rd[x] +
					      rd[xp2]) >> 2;

					switch (bayer_order) {
					case 0:	/* BGGR: [B G] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 d1, n1, c1, h2, c2, v2);
						break;
					case 1:	/* GBRG: [G B] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 h1, c1, v1, d2, n2, c2);
						break;
					case 2:	/* GRBG: [G R] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 h1, c1, v1, c2, n2, d2);
						break;
					default: /* RGGB: [R G] */
						yuv_pair(o + (x - x0) * 2, rg, bg,
							 c1, n1, d1, v2, c2, h2);
						break;
					}
				}
			}
		}

		/* load the row for the NEXT iteration into the slot that held y-1 */
		if (yy + 2 < n)
			memcpy(row[(yy + 2) % 3], in + (yy + 2) * w, w * 2);
	}
}

struct demosaic_job {
	const uint16_t *in;
	uint8_t *out;
	unsigned int w, h, rg, bg, x0, x1, y0, y1;
};

static void *demosaic_thread(void *arg)
{
	struct demosaic_job *j = arg;

	demosaic_bggr10_to_yuyv(j->in, j->out, j->w, j->h, j->rg, j->bg,
				j->x0, j->x1, j->y0, j->y1);
	return NULL;
}

int main(int argc, char **argv)
{
	const char *media = "/dev/media1";
	int setup_only = 0, want_fps = 0, want_exp = 0, want_gain = 0;
	int want_rg = 0, want_bg = 0, awb = 0;
	/*
	 * Highest exposure we may ask for, in lines.  The driver writes
	 * SHS1 = VMAX - exposure but only clamps exposure to VMAX, and
	 * SHS1 = 0 is out of spec: the sensor then emits nothing but the
	 * black pedestal.  Stop 32 lines short of VMAX - that costs 1% of
	 * the frame time, and SHS1 = 14 is known good.  --fps moves VMAX
	 * (and the driver's default vblank is 30), so this is recomputed
	 * as soon as the frame rate is set.
	 */
	int exp_max = SENSOR_H + 30 - 32;
	int stdout_mode = 0, full_frame = 0, a;
	struct media_device_info minfo;
	const struct ent *sensor = NULL, *capture = NULL;
	int mfd, cfd = -1, i, ret, nbuf;
	struct buf bufs[MAXBUF] = { 0 };
	struct v4l2_subdev_format sfmt;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	enum v4l2_buf_type type;
	FILE *stream_out = NULL;
	unsigned int w = 0, h = 0;
	unsigned int code = MEDIA_BUS_FMT_Y10_1X10;
	unsigned int pixfmt = V4L2_PIX_FMT_Y10;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "--setup-only"))
			setup_only = 1;
		else if (!strcmp(argv[a], "--stdout"))
			stdout_mode = 1;
		else if (!strcmp(argv[a], "--full"))
			full_frame = 1;
		else if (!strcmp(argv[a], "--fps"))
			want_fps = (argc > a + 1) ? atoi(argv[++a]) : 0;
		else if (!strcmp(argv[a], "--exposure"))
			want_exp = (argc > a + 1) ? atoi(argv[++a]) : 0;
		else if (!strcmp(argv[a], "--gain"))
			want_gain = (argc > a + 1) ? atoi(argv[++a]) : 0;
		else if (!strcmp(argv[a], "--wb")) {
			want_rg = (argc > a + 1) ? atoi(argv[++a]) : 0;
			want_bg = (argc > a + 1) ? atoi(argv[++a]) : 0;
		} else if (!strcmp(argv[a], "--awb"))
			awb = 1;
		else if (!strcmp(argv[a], "--order")) {
			const char *o = (argc > a + 1) ? argv[++a] : "";

			if (!strcmp(o, "gbrg"))
				bayer_order = 1;
			else if (!strcmp(o, "grbg"))
				bayer_order = 2;
			else if (!strcmp(o, "rggb"))
				bayer_order = 3;
			else
				bayer_order = 0;	/* bggr */
		}
		else
			media = argv[a];
	}

	/*
	 * --stdout: stdout must carry nothing but frame bytes.  Keep the
	 * original pipe fd and point the C stdout at stderr instead, so every
	 * diagnostic below goes to the terminal and the frames are written
	 * through stream_out.
	 */
	if (stdout_mode) {
		int out = dup(STDOUT_FILENO);
		int err = dup(STDERR_FILENO);

		if (out < 0 || err < 0) {
			perror("dup");
			return 1;
		}
		dup2(err, STDOUT_FILENO);
		close(err);
		stream_out = fdopen(out, "w");
		if (!stream_out) {
			perror("fdopen");
			return 1;
		}
		/* one write() per frame instead of ~600 4 KB-chunked writes */
		setvbuf(stream_out, NULL, _IOFBF, SENSOR_W * SENSOR_H * 2);
	}

	mfd = open(media, O_RDWR);
	if (mfd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", media, strerror(errno));
		return 1;
	}
	if (xioctl(mfd, MEDIA_IOC_DEVICE_INFO, &minfo) < 0) {
		perror("MEDIA_IOC_DEVICE_INFO");
		return 1;
	}
	printf("media device: %s / %s\n\n", minfo.driver, minfo.model);

	if (!enumerate(mfd)) {
		fprintf(stderr, "no media entities found\n");
		return 1;
	}

	puts("graph:");
	for (i = 0; i < nents; i++)
		printf("  %-28s dev=%-12s pads=%u links=%u\n", ents[i].name,
		       ents[i].dev[0] ? ents[i].dev : "-",
		       ents[i].d.pads, ents[i].d.links);

	for (i = 0; i < nents; i++)
		if (strstr(ents[i].name, "imx296"))
			sensor = &ents[i];
	if (!sensor) {
		fprintf(stderr, "\nno imx296 sensor entity in the graph\n");
		return 1;
	}
	printf("\nsensor : %s (%s)\n", sensor->name, sensor->dev);

	puts("\nenabling links:");
	enable_links(mfd);

	/*
	 * Formats.  The sensor is authoritative: imx296_set_fmt() forces its bus
	 * code from the module it detected (mono -> Y10, colour -> SBGGR10), so
	 * ask the sensor first and propagate whatever it reports through the rest
	 * of the pipeline.
	 */
	puts("\nformats:");
	{
		int fd = open_dev(sensor), pad;

		if (fd < 0) {
			fprintf(stderr, "  cannot open /dev/%s: %s\n", sensor->dev,
				strerror(errno));
			return 1;
		}
		for (pad = 0; pad < 4; pad++) {
			memset(&sfmt, 0, sizeof(sfmt));
			sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
			sfmt.pad = pad;
			sfmt.format.code = MEDIA_BUS_FMT_Y10_1X10;
			sfmt.format.width = SENSOR_W;
			sfmt.format.height = SENSOR_H;
			sfmt.format.field = V4L2_FIELD_NONE;
			if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &sfmt) < 0)
				continue;
			printf("  %-24s pad%u -> code=%#x %ux%u\n", sensor->name,
			       pad, sfmt.format.code, sfmt.format.width,
			       sfmt.format.height);
			if (pad == 0 && sfmt.format.width && sfmt.format.height) {
				code = sfmt.format.code;
				w = sfmt.format.width;
				h = sfmt.format.height;
			}
		}

		if (want_fps >= 5 && want_fps <= 120) {
			/*
			 * Slow the sensor by adding vertical blanking.  HMAX is
			 * 1100 INCK units = 1760 pixels per line and the pixel
			 * rate is 118.8 MHz, so
			 *     fps = 118800000 / (1760 * (1088 + vblank)).
			 * The driver writes VMAX from vblank->cur.val at
			 * stream on.
			 */
			struct v4l2_control c;
			int vbl = 118800000u / (1760u * want_fps) - SENSOR_H;

			if (vbl < 30)
				vbl = 30;
			memset(&c, 0, sizeof(c));
			c.id = V4L2_CID_VBLANK;
			c.value = vbl;
			if (xioctl(fd, VIDIOC_S_CTRL, &c) == 0) {
				exp_max = SENSOR_H + vbl - 32;
				printf("  %-24s VBLANK -> %d (~%d fps, "
				       "max exposure %d)\n",
				       sensor->name, vbl, want_fps, exp_max);
			} else
				perror("VIDIOC_S_CTRL(VBLANK)");
		}

		if (want_exp > 0) {
			/* Manual exposure, in lines (driver default 1104). */
			struct v4l2_control c;

			memset(&c, 0, sizeof(c));
			c.id = V4L2_CID_EXPOSURE;
			c.value = want_exp;
			if (xioctl(fd, VIDIOC_S_CTRL, &c) == 0)
				printf("  %-24s EXPOSURE -> %d lines\n",
				       sensor->name, c.value);
			else
				perror("VIDIOC_S_CTRL(EXPOSURE)");
		}

		if (want_gain > 0) {
			/* Analogue gain in 0.1 dB steps (0-480). */
			struct v4l2_control c;

			memset(&c, 0, sizeof(c));
			c.id = V4L2_CID_ANALOGUE_GAIN;
			c.value = want_gain;
			if (xioctl(fd, VIDIOC_S_CTRL, &c) == 0)
				printf("  %-24s GAIN -> %d (%.1f dB)\n",
				       sensor->name, c.value, c.value / 10.0);
			else
				perror("VIDIOC_S_CTRL(GAIN)");
		}
		close(fd);
	}
	if (!w || !h) {
		fprintf(stderr, "\ncannot set a format on the sensor\n");
		return 1;
	}

	switch (code) {
	case MEDIA_BUS_FMT_SBGGR10_1X10:
		pixfmt = V4L2_PIX_FMT_SBGGR10;
		break;
	default:			/* mono module */
		pixfmt = V4L2_PIX_FMT_Y10;
		break;
	}
	printf("sensor: code=%#x %ux%u -> pixfmt '%c%c%c%c'\n", code, w, h,
	       pixfmt & 0xff, (pixfmt >> 8) & 0xff, (pixfmt >> 16) & 0xff,
	       (pixfmt >> 24) & 0xff);

	for (i = 0; i < nents; i++) {
		int fd, pad;

		if (!is_subdev(&ents[i]) || &ents[i] == sensor)
			continue;
		fd = open_dev(&ents[i]);
		if (fd < 0) {
			fprintf(stderr, "  cannot open /dev/%s: %s\n",
				ents[i].dev, strerror(errno));
			continue;
		}
		for (pad = 0; pad < 4; pad++) {
			memset(&sfmt, 0, sizeof(sfmt));
			sfmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
			sfmt.pad = pad;
			sfmt.format.code = code;
			sfmt.format.width = w;
			sfmt.format.height = h;
			sfmt.format.field = V4L2_FIELD_NONE;
			if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &sfmt) == 0)
				printf("  %-24s pad%u -> code=%#x %ux%u\n",
				       ents[i].name, pad, sfmt.format.code,
				       sfmt.format.width, sfmt.format.height);
		}
		close(fd);
	}

	/*
	 * Which of the shim's four context video nodes does the sensor reach?
	 * Their names all contain "csi" (ticsi2rx) and all four links are
	 * ENABLED,IMMUTABLE, so matching on name picked context 3 - whose route
	 * is inactive and whose pad carries no format, which makes link
	 * validation fail the stream with -EPIPE.  The graph is the authority:
	 * the shim accepts a format only on the source pad its active route
	 * feeds, so read the source pads back and follow the first one that
	 * carries a format to whichever entity it links to.
	 */
	{
		const struct ent *shim = NULL;

		for (i = 0; i < nents; i++)
			if (is_subdev(&ents[i]) && strstr(ents[i].name, "ticsi2rx") &&
			    !strstr(ents[i].name, "context")) {
				shim = &ents[i];
				break;
			}
		if (shim) {
			int fd = open_dev(shim), pad, routed = -1;

			if (fd >= 0) {
				for (pad = 1; pad < (int)shim->d.pads; pad++) {
					struct v4l2_subdev_format gf;

					memset(&gf, 0, sizeof(gf));
					gf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
					gf.pad = pad;
					if (xioctl(fd, VIDIOC_SUBDEV_G_FMT, &gf) == 0 &&
					    gf.format.width && gf.format.height) {
						routed = pad;
						break;
					}
				}
				close(fd);
			}
			if (routed >= 0) {
				struct media_links_enum links;
				unsigned int l;

				memset(&links, 0, sizeof(links));
				links.entity = shim->d.id;
				if (shim->d.pads)
					links.pads = calloc(shim->d.pads,
							    sizeof(*links.pads));
				if (shim->d.links)
					links.links = calloc(shim->d.links,
							     sizeof(*links.links));
				if ((!shim->d.pads || links.pads) &&
				    (!shim->d.links || links.links) &&
				    xioctl(mfd, MEDIA_IOC_ENUM_LINKS, &links) == 0) {
					for (l = 0; l < shim->d.links; l++) {
						int j;

						if (links.links[l].source.entity != shim->d.id ||
						    links.links[l].source.index != (unsigned)routed)
							continue;
						for (j = 0; j < nents; j++)
							if (ents[j].d.id == links.links[l].sink.entity)
								capture = &ents[j];
					}
				}
				free(links.pads);
				free(links.links);
				if (capture)
					printf("  sensor's stream is routed to shim pad%u -> %s\n",
					       routed, capture->name);
			}
		}
		if (!capture)
			for (i = 0; i < nents; i++)
				if (is_video(&ents[i]) && strstr(ents[i].name, "ticsi2rx") &&
				    !strstr(ents[i].name, "context")) {
					capture = &ents[i];
					break;
				}
	}
	if (!capture) {
		fprintf(stderr, "\nno CSI capture video node in the graph\n");
		return 1;
	}
	printf("\ncapture: %s (%s)\n", capture->name, capture->dev);

	if (setup_only) {
		printf("\nsetup done - graph is configured, nothing is streaming.\n");
		printf("for a live stream use --stdout, e.g.:\n");
		printf("  imx296-capture --stdout --fps 30 /dev/media1 | gst-launch-1.0 -q fdsrc ! \\\n");
		printf("    video/x-raw,format=YUY2,width=%u,height=%u,framerate=30/1 ! \\\n", w, h);
		printf("    videocrop top=134 bottom=136 ! videoscale ! video/x-raw,width=1280,height=720 ! \\\n");
		printf("    videoconvert ! jpegenc quality=75 ! rtpjpegpay ! \\\n");
		printf("    udpsink host=192.168.1.106 port=5000 sync=false async=false\n");
		close(mfd);
		return 0;
	}

	cfd = open_dev(capture);
	if (cfd < 0) {
		fprintf(stderr, "cannot open /dev/%s: %s\n", capture->dev,
			strerror(errno));
		return 1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = w;
	fmt.fmt.pix.height = h;
	fmt.fmt.pix.pixelformat = pixfmt;
	fmt.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(cfd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("VIDIOC_S_FMT");
		return 1;
	}
	printf("capture format: '%c%c%c%c' %ux%u sizeimage=%u\n",
	       fmt.fmt.pix.pixelformat & 0xff,
	       (fmt.fmt.pix.pixelformat >> 8) & 0xff,
	       (fmt.fmt.pix.pixelformat >> 16) & 0xff,
	       (fmt.fmt.pix.pixelformat >> 24) & 0xff,
	       fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.sizeimage);

	memset(&req, 0, sizeof(req));
	req.count = MAXBUF;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	/*
	 * DMABUF from /dev/dma_heap/linux,cma: contiguous (the shim's DMA
	 * rejects scattered system-heap memory with "contiguous chunk is
	 * too small") AND cached, so reads run at cache speed.  With MMAP
	 * the DMA buffers read ~25 ms per 3.17 MB pass, which alone ate
	 * most of the 33 ms frame budget at 30 fps.
	 */
	req.memory = V4L2_MEMORY_DMABUF;
	if (xioctl(cfd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		return 1;
	}
	nbuf = req.count;
	if (nbuf > MAXBUF)
		nbuf = MAXBUF;
	printf("buffers: %u\n", req.count);

	{
		int heapfd;

		heapfd = open("/dev/dma_heap/linux,cma", O_RDWR);
		if (heapfd < 0)
			perror("open /dev/dma_heap/linux,cma");
		for (i = 0; i < nbuf; i++) {
			struct dma_heap_allocation_data alloc = {
				.len = fmt.fmt.pix.sizeimage,
				.fd_flags = O_RDWR | O_CLOEXEC,
			};
			struct v4l2_buffer b;

			bufs[i].fd = -1;
			bufs[i].length = fmt.fmt.pix.sizeimage;
			if (heapfd >= 0 &&
			    xioctl(heapfd, DMA_HEAP_IOCTL_ALLOC, &alloc) == 0) {
				bufs[i].fd = alloc.fd;
				bufs[i].start = mmap(NULL, bufs[i].length,
						     PROT_READ | PROT_WRITE,
						     MAP_SHARED, bufs[i].fd, 0);
			}
			if (!bufs[i].start || bufs[i].start == MAP_FAILED) {
				/* fall back to the shim's own mmap buffers */
				bufs[i].start = MAP_FAILED;
			}
			if (bufs[i].start == MAP_FAILED) {
				fprintf(stderr, "dmabuf allocation failed - falling back to MMAP\n");
				if (bufs[i].fd >= 0) {
					close(bufs[i].fd);
					bufs[i].fd = -1;
				}
				req.memory = V4L2_MEMORY_MMAP;
				if (xioctl(cfd, VIDIOC_REQBUFS, &req) < 0) {
					perror("VIDIOC_REQBUFS(MMAP)");
					return 1;
				}
				break;
			}
			memset(&b, 0, sizeof(b));
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			b.memory = V4L2_MEMORY_DMABUF;
			b.index = i;
			b.m.fd = bufs[i].fd;
			b.length = bufs[i].length;
			if (xioctl(cfd, VIDIOC_QBUF, &b) < 0) {
				perror("VIDIOC_QBUF");
				return 1;
			}
		}
		if (heapfd >= 0)
			close(heapfd);
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(cfd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON");
		return 1;
	}
	puts("streaming...");

	if (stdout_mode) {
		/*
		 * Crop to the stream geometry (1280x720, see STREAM_W) before
		 * demosaicing: the pipeline throws those pixels away anyway,
		 * the crop keeps the bayer phase (both offsets are even), and
		 * skipping the math is ~25% of the demosaic.  --full keeps
		 * the whole frame.
		 */
		unsigned int x0 = 0, x1 = w, y0 = 0, y1 = h;

		if (!full_frame) {
			x0 = CROP_X0;
			x1 = CROP_X0 + STREAM_W;
			y0 = CROP_Y0;
			y1 = CROP_Y0 + STREAM_H;
		}
		{
			uint8_t *yuyv = malloc((y1 - y0) * (x1 - x0) * 2);
			unsigned long frames = 0;
			unsigned int rg = 1024, bg = 1024;	/* WB gains */
			unsigned int mean = 0;
			int exp_ctl = 1104;	/* the driver's exposure default */
			int gain_ctl = 0;
			/* consecutive out-of-band AE readings, signed */
			int ae_streak = 0;
			int sfd = open_dev(sensor);

			/*
			 * Start from wherever the last run left the sensor,
			 * so the AE's idea of exposure/gain matches the
			 * hardware - but never inherit an out-of-range
			 * exposure, since that is precisely the state that
			 * yields black frames.
			 */
			if (sfd >= 0) {
				struct v4l2_control c;

				memset(&c, 0, sizeof(c));
				c.id = V4L2_CID_EXPOSURE;
				if (xioctl(sfd, VIDIOC_G_CTRL, &c) == 0 &&
				    c.value >= 60 && c.value <= exp_max)
					exp_ctl = c.value;
				memset(&c, 0, sizeof(c));
				c.id = V4L2_CID_ANALOGUE_GAIN;
				if (xioctl(sfd, VIDIOC_G_CTRL, &c) == 0 &&
				    c.value >= 0 && c.value <= 480)
					gain_ctl = c.value;
			}

		if (!yuyv) {
			perror("malloc");
			return 1;
		}
		/* A broken pipe is a normal end of stream: report, don't die. */
		signal(SIGPIPE, SIG_IGN);
		/* Write YUYV frames until the pipe breaks (gstreamer exited). */
		for (;;) {
			struct v4l2_buffer b;
			struct pollfd pfd = { .fd = cfd, .events = POLLIN };

			ret = poll(&pfd, 1, 5000);
			if (ret <= 0) {
				fprintf(stderr, "poll: %s\n",
					ret < 0 ? strerror(errno) : "timeout");
				break;
			}
			memset(&b, 0, sizeof(b));
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			b.memory = V4L2_MEMORY_DMABUF;
			if (xioctl(cfd, VIDIOC_DQBUF, &b) < 0) {
				perror("VIDIOC_DQBUF");
				break;
			}
			if (!want_rg && awb && frames % 15 == 14 && wb_cnt) {
				/*
				 * Output-side gray-world: the gains come from the
				 * DEMOSAIC's actual channel means (accumulated in
				 * yuv_pair), so the loop absorbs whatever the
				 * demosaic does - bayer order, interpolation
				 * bias, everything.  Recomputes every 30 frames;
				 * --wb RG BG (1024 = 1.0) overrides entirely.
				 */
				unsigned long long mr = wb_sum_r / wb_cnt;
				unsigned long long mg = wb_sum_g / wb_cnt;
				unsigned long long mb = wb_sum_b / wb_cnt;

				/*
				 * Full-step multiplicative update: the measured
				 * R already includes the current gain, so
				 * g * G/R cancels it and converges in ONE step
				 * (the additive form oscillated; the half-step
				 * lagged ~10% behind moving scenes).  The
				 * 15-frame averaging smooths the noise.
				 */
				if (mr && mb) {
					rg = (unsigned int)((unsigned long long)rg *
							    mg / mr);
					bg = (unsigned int)((unsigned long long)bg *
							    mg / mb);
				}
				if (rg < 128)
					rg = 128;
				if (rg > 4096)
					rg = 4096;
				if (bg < 128)
					bg = 128;
				if (bg > 4096)
					bg = 4096;
				fprintf(stderr,
					"wb: rg=%u bg=%u [out R=%llu G=%llu B=%llu]\n",
					rg, bg, mr, mg, mb);
				wb_sum_r = wb_sum_g = wb_sum_b = wb_cnt = 0;
			}
			if (want_rg && frames == 0) {
				rg = want_rg;
				bg = want_bg;
				fprintf(stderr, "wb: manual rg=%u bg=%u\n",
					rg, bg);
			}
			if (!want_exp && frames % 30 == 29 && exp_ctl > 0) {
				/*
				 * Auto-exposure, in two stages: walk the
				 * exposure control toward a raw mean of
				 * ~500/1023, then bring in analogue gain once
				 * exposure runs out.  The driver writes
				 * SHS1 = VMAX - exposure, so higher = brighter,
				 * but it only clamps exposure to VMAX - and
				 * SHS1 = 0 is out of spec, so the sensor emits
				 * nothing but the black pedestal.  Hence
				 * exp_max rather than a flat 4000: at 30 fps
				 * VMAX is 2250 and the old ceiling walked the
				 * sensor straight off that cliff.  Only runs
				 * when no manual --exposure is given.
				 *
				 * Glass and shiny surfaces used to make the
				 * whole frame flash, in three ways:
				 *
				 *  - Pixels at 950/1023 or above are left out
				 *    of the mean.  A specular highlight is a
				 *    small patch of blown pixels carrying no
				 *    exposure information, but it used to drag
				 *    the whole-frame mean up and swing the
				 *    loop.  Dropping it means the loop exposes
				 *    for the scene behind the reflection.
				 *  - The error has to persist before anything
				 *    moves, so a highlight or a hand passing
				 *    through cannot shift the exposure.  A huge
				 *    error is exempt: half a frame of blown
				 *    pixels is not an accident, that is a real
				 *    lighting change and waiting only makes the
				 *    loop slow.
				 *  - Steps are sized by the error and are
				 *    small: ~1.05x near the band, ~1.3x at the
				 *    far end.  gain_ctl counts 0.1 dB, i.e. it
				 *    is already logarithmic, so it has to move
				 *    by a constant number of units, not by a
				 *    fraction of itself: the old gain/3 step
				 *    was a 2.4x brightness jump once the room
				 *    was dim enough to need gain, which is
				 *    exactly the flash this fix is about.
				 */
				const uint16_t *px = bufs[b.index].start;
				unsigned long long sum = 0;
				unsigned long cnt = 0;
				unsigned int k, err = 0, step;
				int bright = 0, changed = 0;

				for (k = 0; k < w * h; k += 17) {
					unsigned int v = px[k] & 0x3ff;

					if (v <= 950) {
						sum += v;
						cnt++;
					}
				}
				/*
				 * Nothing but blown pixels left: the frame
				 * really is over-exposed.  Report 1023 so
				 * the loop backs off instead of stalling on
				 * an empty sample.
				 */
				mean = cnt ? (unsigned int)(sum / cnt) : 1023;

				if (mean < 420) {
					err = 420 - mean;
				} else if (mean > 620) {
					err = mean - 620;
					bright = 1;
				}
				if (!err) {
					ae_streak = 0;
				} else if (bright) {
					ae_streak = ae_streak < 0 ?
						    ae_streak - 1 : -1;
				} else {
					ae_streak = ae_streak > 0 ?
						    ae_streak + 1 : 1;
				}
				if (err && (err > 200 ||
					    ae_streak >= 2 || ae_streak <= -2)) {
					/*
					 * Percent of brightness to add or
					 * remove this second.  Even the
					 * largest is under the band's own
					 * width (620/420 = 1.48x), so one
					 * correction can never jump clean
					 * over the target and come back at
					 * it from the other side.  Only an
					 * error past 200 - a real lighting
					 * change, not a reflection - gets
					 * the big step.
					 */
					step = err > 200 ? 32 : err / 8;
					if (step < 5)
						step = 5;
					if (step > 32)
						step = 32;
					if (!bright) {
						if (exp_ctl < exp_max) {
							exp_ctl += exp_ctl *
								  step / 100 + 8;
							if (exp_ctl > exp_max)
								exp_ctl = exp_max;
							changed = 1;
						} else if (!want_gain &&
							   gain_ctl < 480) {
							/*
							 * Exposure is maxed
							 * and we are still
							 * dark: spend gain.
							 * 4/5 converts the
							 * percent above into
							 * 0.1 dB units, since
							 * a gain of x is
							 * 200*log10(x) here.
							 */
							gain_ctl += step * 4 / 5;
							if (gain_ctl > 480)
								gain_ctl = 480;
							changed = 2;
						}
					} else if (!want_gain && gain_ctl > 0) {
						/*
						 * Too bright: shed gain
						 * first, it is the
						 * noisier of the two
						 * controls.
						 */
						gain_ctl -= step * 4 / 5;
						if (gain_ctl < 0)
							gain_ctl = 0;
						changed = 2;
					} else if (exp_ctl > 60) {
						exp_ctl -= exp_ctl *
							  step / 100 + 8;
						if (exp_ctl < 60)
							exp_ctl = 60;
						changed = 1;
					}
				}
				if (changed) {
					struct v4l2_control c;

					memset(&c, 0, sizeof(c));
					c.id = changed == 2 ? V4L2_CID_ANALOGUE_GAIN
							    : V4L2_CID_EXPOSURE;
					c.value = changed == 2 ? gain_ctl : exp_ctl;
					if (sfd >= 0 && xioctl(sfd, VIDIOC_S_CTRL, &c) == 0)
						fprintf(stderr,
							"ae: mean=%u exp=%d gain=%d\n",
							mean, exp_ctl, gain_ctl);
				} else if (frames % 300 == 29) {
					fprintf(stderr,
						"ae: mean=%u exp=%d gain=%d (steady)\n",
						mean, exp_ctl, gain_ctl);
				}
			}
			{
				pthread_t t1, t2;
				struct demosaic_job j1, j2;
				unsigned int mid = y0 + (y1 - y0) / 2;

				j1.in = bufs[b.index].start;
				j1.out = yuyv;
				j1.w = w;
				j1.h = h;
				j1.rg = rg;
				j1.bg = bg;
				j1.x0 = x0;
				j1.x1 = x1;
				j1.y0 = y0;
				j1.y1 = mid;
				j2 = j1;
				j2.out = yuyv + (mid - y0) * (x1 - x0) * 2;
				j2.y0 = mid;
				j2.y1 = y1;
				pthread_create(&t1, NULL, demosaic_thread, &j1);
				pthread_create(&t2, NULL, demosaic_thread, &j2);
				pthread_join(t1, NULL);
				pthread_join(t2, NULL);
			}
			if (fwrite(yuyv, (y1 - y0) * (x1 - x0) * 2, 1,
				   stream_out) != 1) {
				fprintf(stderr, "stdout closed after %lu frames\n",
					frames);
				break;
			}
			fflush(stream_out);
			frames++;
			b.m.fd = bufs[b.index].fd;
			b.length = bufs[b.index].length;
			if (xioctl(cfd, VIDIOC_QBUF, &b) < 0) {
				perror("VIDIOC_QBUF");
				break;
			}
		}
		free(yuyv);
			if (sfd >= 0)
				close(sfd);
			fclose(stream_out);
			for (i = 0; i < nbuf; i++) {
				if (bufs[i].start && bufs[i].start != MAP_FAILED)
					munmap(bufs[i].start, bufs[i].length);
				if (bufs[i].fd >= 0)
					close(bufs[i].fd);
			}
			type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			xioctl(cfd, VIDIOC_STREAMOFF, &type);
			return 0;
		}
	}

	for (i = 0; i < 5; i++) {
		struct v4l2_buffer b;
		struct pollfd pfd = { .fd = cfd, .events = POLLIN };

		ret = poll(&pfd, 1, 5000);
		if (ret < 0) {
			perror("poll");
			break;
		}
		if (!ret) {
			fprintf(stderr, "timeout waiting for frame %d\n", i);
			break;
		}
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_DMABUF;
		if (xioctl(cfd, VIDIOC_DQBUF, &b) < 0) {
			perror("VIDIOC_DQBUF");
			break;
		}

		if (i == 0) {
			const uint16_t *px = bufs[b.index].start;
			unsigned int n = b.bytesused / 2, k;
			uint32_t sum = 0;
			uint16_t mn = 0xffff, mx = 0;

			for (k = 0; k < n; k++) {
				sum += px[k];
				if (px[k] < mn)
					mn = px[k];
				if (px[k] > mx)
					mx = px[k];
			}
			printf("frame0: %u bytes, %u px, min=%u max=%u mean=%u\n",
			       b.bytesused, n, n ? mn : 0, mx, n ? sum / n : 0);
			{
				/* raw channel means (BGGR): the WB ground truth */
				unsigned long long rs = 0, gs = 0, bs = 0;
				unsigned int yy, xx;

				for (yy = 0; yy < h; yy++)
					for (xx = 0; xx < w; xx++) {
						unsigned int v = px[yy * w + xx] & 0x3ff;

						if (yy & 1) {
							if (xx & 1)
								rs += v;
							else
								gs += v;
						} else {
							if (xx & 1)
								gs += v;
							else
								bs += v;
						}
					}
				printf("  raw R=%llu G=%llu B=%llu (10-bit means)\n",
				       rs / (w * h / 4), gs / (w * h / 2),
				       bs / (w * h / 4));
			}
			printf("first 16 px:");
			for (k = 0; k < 16 && k < n; k++)
				printf(" %u", px[k]);
			printf("\n");
		} else {
			printf("frame%d: %u bytes, seq=%u\n", i, b.bytesused,
			       b.sequence);
		}
		b.m.fd = bufs[b.index].fd;
		b.length = bufs[b.index].length;
		if (xioctl(cfd, VIDIOC_QBUF, &b) < 0) {
			perror("VIDIOC_QBUF");
			break;
		}
	}

	for (i = 0; i < nbuf; i++) {
		if (bufs[i].start && bufs[i].start != MAP_FAILED)
			munmap(bufs[i].start, bufs[i].length);
		if (bufs[i].fd >= 0)
			close(bufs[i].fd);
	}
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(cfd, VIDIOC_STREAMOFF, &type);
	return 0;
}
