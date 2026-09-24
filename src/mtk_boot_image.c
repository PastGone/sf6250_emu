/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MediaTek MT6250 (SF_BOOT) 闪存镜像解析器
 *
 * 参见 mtk_boot_image.h 中的布局说明。
 */

#include "mtk_boot_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxi-log/log.h"

/* 结构体必须与 on-flash 布局逐字节对应（自然对齐下无填充） */
_Static_assert(sizeof(union gen_boot_header) == 0x200, "gen_boot_header size");
_Static_assert(sizeof(struct brom_layout_header) == 0xb4, "brom_layout_header size");
_Static_assert(sizeof(struct brom_layout_region) == BRLYT_REGION_SIZE,
	       "brom_layout_region size");
_Static_assert(sizeof(struct gfh_common_header) == 8, "gfh_common_header size");
_Static_assert(sizeof(struct gfh_file_info) == 0x38, "gfh_file_info size");
_Static_assert(sizeof(struct gfh_bl_info) == 0xc, "gfh_bl_info size");
_Static_assert(sizeof(struct gfh_brom_cfg) == 0x64, "gfh_brom_cfg size");
_Static_assert(sizeof(struct gfh_bl_sec_key) == 0x214, "gfh_bl_sec_key size");
_Static_assert(sizeof(struct gfh_anti_clone) == 0x14, "gfh_anti_clone size");
_Static_assert(sizeof(struct gfh_brom_sec_cfg) == 0x30, "gfh_brom_sec_cfg size");

#define BRLYT_OFFSET	0x200u

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 拷贝定长字符串：遇到 NUL 结束，非可打印字符替换成 '.'，并去掉尾部空格 */
static void fixed_str(char *dst, size_t dstsz, const uint8_t *src, size_t n)
{
	size_t i;

	if (dstsz == 0)
		return;
	if (n > dstsz - 1)
		n = dstsz - 1;

	for (i = 0; i < n; i++) {
		uint8_t c = src[i];

		if (c == 0)
			break;
		dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	while (i > 0 && dst[i - 1] == ' ')
		i--;
	dst[i] = '\0';
}

/* GFH 体里只有一部分以可读名字开头（FILE_INFO / EMI_INF / 工程名），
 * 其余是二进制参数，这里判断一下避免出现 "...." 这种假名字。 */
static int looks_like_str(const uint8_t *p, size_t n)
{
	size_t i, cnt = 0;

	if (n == 0 || p[0] < 0x20 || p[0] >= 0x7f)
		return 0;
	for (i = 0; i < n && p[i] != 0; i++) {
		if (p[i] >= 0x20 && p[i] < 0x7f)
			cnt++;
	}
	return cnt >= 2;
}

static int gfh_magic_ok(const uint8_t *p, size_t avail)
{
	return avail >= sizeof(struct gfh_common_header) &&
	       p[0] == 'M' && p[1] == 'M' && p[2] == 'M';
}

static void gfh_read(const uint8_t *p, struct gfh_common_header *h)
{
	memcpy(h->magic, p, 3);
	h->version = p[3];
	h->size = le16(p + 4);
	h->type = le16(p + 6);
}

static void file_info_read(const uint8_t *p, struct gfh_file_info *f)
{
	gfh_read(p, &f->gfh);
	fixed_str(f->name, sizeof(f->name), p + 8, 12);
	f->unused = le32(p + 20);
	f->file_type = le16(p + 24);
	f->flash_type = p[26];
	f->sig_type = p[27];
	f->load_addr = le32(p + 28);
	f->total_size = le32(p + 32);
	f->max_size = le32(p + 36);
	f->hdr_size = le32(p + 40);
	f->sig_size = le32(p + 44);
	f->jump_offset = le32(p + 48);
	f->processed = le32(p + 52);
}

/* FILE_INFO 的合法性判断，用于全文件扫描时排除误命中 */
static int file_info_valid(const uint8_t *p, size_t avail)
{
	if (avail < sizeof(struct gfh_file_info))
		return 0;
	if (!gfh_magic_ok(p, avail))
		return 0;
	if (le16(p + 4) != sizeof(struct gfh_file_info))
		return 0;
	if (le16(p + 6) != GFH_TYPE_FILE_INFO)
		return 0;
	if (memcmp(p + 8, GFH_FILE_INFO_NAME, sizeof(GFH_FILE_INFO_NAME) - 1) != 0)
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* 镜像表                                                              */
/* ------------------------------------------------------------------ */

static struct mtk_image *image_find_slot(struct mtk_boot_image *img, uint32_t off)
{
	unsigned int i;

	for (i = 0; i < img->image_count; i++) {
		if (img->image[i].offset == off)
			return &img->image[i];
	}
	return NULL;
}

/* 遍历一个镜像的 GFH 链，直到 hdr_size 结束 */
static void image_collect_gfh(struct mtk_boot_image *img, struct mtk_image *im)
{
	const uint8_t *base = img->data;
	uint32_t cur = im->offset + sizeof(struct gfh_file_info);
	uint32_t end = im->offset + im->hdr_size;

	if (im->hdr_size < sizeof(struct gfh_file_info) || end > img->size)
		return;

	while (cur + sizeof(struct gfh_common_header) <= end &&
	       im->gfh_count < MTK_IMAGE_MAX_GFH) {
		struct mtk_gfh_entry *e = &im->gfh[im->gfh_count];
		struct gfh_common_header h;

		if (!gfh_magic_ok(base + cur, img->size - cur))
			break;
		gfh_read(base + cur, &h);
		if (h.size < sizeof(struct gfh_common_header) ||
		    cur + h.size > end)
			break;

		e->offset = cur;
		e->type = h.type;
		e->size = h.size;
		e->version = h.version;
		if (looks_like_str(base + cur + 8, h.size - 8))
			fixed_str(e->name, sizeof(e->name), base + cur + 8,
				  h.size - 8);

		if (h.type == GFH_TYPE_PROJECT_INFO &&
		    looks_like_str(base + cur + 8, h.size - 8))
			fixed_str(img->project, sizeof(img->project),
				  base + cur + 8, h.size - 8);

		im->gfh_count++;
		cur += h.size;
	}
}

/* 登记（并解析）一个位于 off 处的镜像；off 重复时直接复用已有条目 */
static int image_add(struct mtk_boot_image *img, uint32_t off)
{
	struct gfh_file_info fi;
	struct mtk_image *im;
	unsigned int i;

	if (img->image_count >= MTK_IMAGE_MAX_IMAGES)
		return 0;
	if ((size_t)off + sizeof(struct gfh_file_info) > img->size)
		return 0;
	if (!file_info_valid(img->data + off, img->size - off))
		return 0;

	file_info_read(img->data + off, &fi);
	if (fi.total_size == 0 || (size_t)off + fi.total_size > img->size)
		return 0;

	if (image_find_slot(img, off))
		return 0;	/* 已登记 */

	im = &img->image[img->image_count++];
	memset(im, 0, sizeof(*im));

	im->offset = off;
	im->total_size = fi.total_size;
	im->end = off + fi.total_size;
	im->hdr_size = fi.hdr_size;
	im->sig_size = fi.sig_size;
	im->jump_offset = fi.jump_offset;
	im->load_addr = fi.load_addr;
	im->entry_point = fi.load_addr + fi.jump_offset;
	im->file_type = fi.file_type;
	im->flash_type = fi.flash_type;
	im->sig_type = fi.sig_type;
	im->processed = fi.processed;
	fixed_str(im->name, sizeof(im->name), img->data + off + 8, 12);

	image_collect_gfh(img, im);

	/* 保持按 offset 升序 */
	for (i = img->image_count - 1; i > 0; i--) {
		if (img->image[i - 1].offset <= img->image[i].offset)
			break;
		struct mtk_image tmp = img->image[i];
		img->image[i] = img->image[i - 1];
		img->image[i - 1] = tmp;
	}
	return 1;
}

/* BRLYT 只描述 bootloader 部分，其余镜像（MAUI 等）靠全文件扫描补齐 */
static void image_scan_all(struct mtk_boot_image *img)
{
	size_t off = 0;

	while (off + sizeof(struct gfh_file_info) <= img->size &&
	       img->image_count < MTK_IMAGE_MAX_IMAGES) {
		if (file_info_valid(img->data + off, img->size - off))
			image_add(img, (uint32_t)off);
		off++;
	}
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

const char *mtk_file_type_name(uint16_t file_type)
{
	switch (file_type) {
	case GFH_FILE_TYPE_BL:
		return "BL (1st bootloader)";
	case GFH_FILE_TYPE_EXT_BL:
		return "ExtBL (2nd bootloader)";
	case GFH_FILE_TYPE_MAUI:
		return "MAUI (main firmware)";
	case GFH_FILE_TYPE_MAUI_108:
		return "MAUI resource / 2nd image";
	default:
		return "unknown";
	}
}

const char *mtk_gfh_type_name(uint16_t type)
{
	switch (type) {
	case GFH_TYPE_FILE_INFO:
		return "FILE_INFO";
	case GFH_TYPE_BL_INFO:
		return "BL_INFO";
	case GFH_TYPE_ANTI_CLONE:
		return "ANTI_CLONE";
	case GFH_TYPE_BL_SEC_KEY:
		return "BL_SEC_KEY";
	case GFH_TYPE_BROM_CFG:
		return "BROM_CFG";
	case GFH_TYPE_BROM_SEC_CFG:
		return "BROM_SEC_CFG";
	case GFH_TYPE_EMI_INFO:
		return "EMI_INFO";
	case GFH_TYPE_PROJECT_INFO:
		return "PROJECT_INFO";
	default:
		return "unknown";
	}
}

int mtk_boot_image_load_file(const char *path, uint8_t **buf, size_t *len)
{
	FILE *fp;
	uint8_t *p;
	long sz;

	if (!path || !buf || !len)
		return MTK_IMG_ERR_IO;

	fp = fopen(path, "rb");
	if (!fp)
		return MTK_IMG_ERR_IO;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return MTK_IMG_ERR_IO;
	}
	sz = ftell(fp);
	if (sz < 0) {
		fclose(fp);
		return MTK_IMG_ERR_IO;
	}
	rewind(fp);

	p = malloc((size_t)sz);
	if (!p) {
		fclose(fp);
		return MTK_IMG_ERR_NOMEM;
	}
	if (fread(p, 1, (size_t)sz, fp) != (size_t)sz) {
		free(p);
		fclose(fp);
		return MTK_IMG_ERR_IO;
	}
	fclose(fp);

	*buf = p;
	*len = (size_t)sz;
	return MTK_IMG_OK;
}

int mtk_boot_image_parse(const void *buf, size_t len, struct mtk_boot_image *out)
{
	const uint8_t *p = buf;
	const uint8_t *b;
	const uint8_t *r;
	unsigned int i;

	if (!buf || !out)
		return MTK_IMG_ERR_TOO_SMALL;

	memset(out, 0, sizeof(*out));

	if (len < BRLYT_OFFSET + sizeof(struct brom_layout_header))
		return MTK_IMG_ERR_TOO_SMALL;

	out->data = p;
	out->size = len;

	/* 1. 设备头 */
	fixed_str(out->boot.name, sizeof(out->boot.name), p + 0, 12);
	out->boot.version = le32(p + 12);
	out->boot.size = le32(p + 16);

	if (strcmp(out->boot.name, SF_BOOT_NAME) != 0 &&
	    strcmp(out->boot.name, EMMC_BOOT_NAME) != 0 &&
	    strcmp(out->boot.name, SDMMC_BOOT_NAME) != 0)
		return MTK_IMG_ERR_BAD_BOOT;

	/* 2. BRLYT 布局头 + region 表 */
	b = p + BRLYT_OFFSET;
	fixed_str(out->brlyt.name, sizeof(out->brlyt.name), b + 0, 8);
	out->brlyt.version = le32(b + 8);
	out->brlyt.header_size = le32(b + 12);
	out->brlyt.total_size = le32(b + 16);

	for (i = 0; i < BRLYT_MAX_REGIONS; i++) {
		struct brom_layout_region *rg = &out->brlyt.region[i];

		r = b + 20 + i * BRLYT_REGION_SIZE;
		rg->magic = le32(r + 0);
		rg->type = le32(r + 4);
		rg->header_size_2 = le32(r + 8);
		rg->total_size_2 = le32(r + 12);
		rg->unused = le32(r + 16);

		if (rg->magic == BRLYT_MAGIC)
			out->brlyt_region_count++;
	}

	if (strcmp(out->brlyt.name, BRLYT_NAME) != 0 ||
	    out->brlyt.region[0].magic != BRLYT_MAGIC)
		return MTK_IMG_ERR_BAD_BRLYT;

	/* 3. BRLYT 指出的镜像（高 16 位为 0xfffe 的槽位不是文件内偏移） */
	for (i = 0; i < BRLYT_MAX_REGIONS; i++) {
		const struct brom_layout_region *rg = &out->brlyt.region[i];

		if (rg->magic != BRLYT_MAGIC)
			continue;
		if (BRLYT_REGION_INDEX(rg->type) == BRLYT_INDEX_RESERVED)
			continue;
		image_add(out, rg->header_size_2);
	}

	/* 4. 扫描补齐剩余的镜像 */
	image_scan_all(out);

	return MTK_IMG_OK;
}

const struct mtk_image *mtk_boot_image_find(const struct mtk_boot_image *img,
					    uint16_t file_type)
{
	unsigned int i;

	if (!img)
		return NULL;
	for (i = 0; i < img->image_count; i++) {
		if (img->image[i].file_type == file_type)
			return &img->image[i];
	}
	return NULL;
}

const uint8_t *mtk_image_payload(const struct mtk_boot_image *img,
				 const struct mtk_image *im, size_t *len)
{
	if (!img || !im || im->hdr_size >= im->total_size ||
	    (size_t)im->end > img->size) {
		if (len)
			*len = 0;
		return NULL;
	}
	if (len)
		*len = im->total_size - im->hdr_size;
	return img->data + im->offset + im->hdr_size;
}

void mtk_boot_image_dump(const struct mtk_boot_image *img)
{
	unsigned int i, j;

	if (!img) {
		log_error("mtk_boot_image_dump: img == NULL");
		return;
	}

	log_info("MTK flash image: %zu bytes (0x%zx)", img->size, img->size);
	log_info("  device header @0x000: name=\"%s\" version=%u size=0x%x",
		 img->boot.name, img->boot.version, img->boot.size);
	log_info("  BRLYT @0x200: name=\"%s\" version=%u header_size=0x%x total_size=0x%x (%u/%d regions used)",
		 img->brlyt.name, img->brlyt.version, img->brlyt.header_size,
		 img->brlyt.total_size, img->brlyt_region_count,
		 BRLYT_MAX_REGIONS);

	if (img->project[0])
		log_info("  project: \"%s\"", img->project);

	for (i = 0; i < BRLYT_MAX_REGIONS; i++) {
		const struct brom_layout_region *rg = &img->brlyt.region[i];
		const struct mtk_image *im;

		if (rg->magic != BRLYT_MAGIC) {
			log_info("    region[%u]: <empty>", i);
			continue;
		}

		im = NULL;
		for (j = 0; j < img->image_count; j++) {
			if (img->image[j].offset == rg->header_size_2)
				im = &img->image[j];
		}

		log_info("    region[%u]: index=0x%04x flash=0x%04x start=0x%08x end=0x%08x flags=0x%08x -> %s",
			 i, BRLYT_REGION_INDEX(rg->type),
			 BRLYT_REGION_FLASH(rg->type), rg->header_size_2,
			 rg->total_size_2, rg->unused,
			 im ? mtk_file_type_name(im->file_type)
			    : (BRLYT_REGION_INDEX(rg->type) ==
			       BRLYT_INDEX_RESERVED ? "<reserved>"
						    : "<unresolved>"));
	}

	log_info("  images: %u", img->image_count);
	for (i = 0; i < img->image_count; i++) {
		const struct mtk_image *im = &img->image[i];
		size_t pay_len = 0;

		mtk_image_payload(img, im, &pay_len);
		log_info("    [%u] @0x%08x..0x%08x size=0x%08x type=0x%03x (%s) load=0x%08x entry=0x%08x jump=0x%x sig=%u flash=%u payload=0x%zx",
			 i, im->offset, im->end, im->total_size, im->file_type,
			 mtk_file_type_name(im->file_type), im->load_addr,
			 im->entry_point, im->jump_offset, im->sig_type,
			 im->flash_type, pay_len);

		for (j = 0; j < im->gfh_count; j++) {
			const struct mtk_gfh_entry *e = &im->gfh[j];

			log_info("        gfh @0x%08x type=0x%04x (%-12s) size=0x%03x ver=%u %s%s%s",
				 e->offset, e->type,
				 mtk_gfh_type_name(e->type), e->size,
				 e->version,
				 e->name[0] ? "name=\"" : "",
				 e->name[0] ? e->name : "",
				 e->name[0] ? "\"" : "");
		}
	}
}
