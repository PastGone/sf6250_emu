/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * MediaTek MT6250 (SF_BOOT) 闪存镜像结构定义
 *
 * 字段名沿用 docs/mtk_image.h.txt（U-Boot 的 mtk_image.h），
 * 具体布局按 res/AHONG50_11B_PCB01_gprs_MT6250_S00.D27D__11B_W11_32__V0010.bin
 * 实测反推。所有 on-flash 字段均为小端。
 *
 * 该 bin 的整体布局（16MB NOR/SF flash dump）：
 *
 *   0x000000  union gen_boot_header      "SF_BOOT"        0x200
 *   0x000200  struct brom_layout_header  "BRLYT"          0x0b4
 *   0x000800  镜像#0  BL      file_type 0x001  load 0x70008000   size 0x002180 -> 0x002980
 *   0x008000  镜像#1  ExtBL   file_type 0x002  load 0x10008000   size 0x0062a0 -> 0x00e2a0
 *   0x020000  镜像#2  MAUI    file_type 0x100  load 0x10020000   size 0x854eb0 -> 0x874eb0
 *   0x8752b0  镜像#3  (资源)  file_type 0x108  load 0x108752b0   size 0x563748 -> 0xdd89f8
 *   0xdd89f8 .. 0x1000000  剩余部分（文件系统 / NVRAM / FOTA 等）
 */

#ifndef _MTK_BOOT_IMAGE_H
#define _MTK_BOOT_IMAGE_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* 1. 设备头（文件偏移 0x000，大小 0x200）                             */
/* ------------------------------------------------------------------ */

union gen_boot_header {
	struct {
		char name[12];
		uint32_t version;
		uint32_t size;
	};

	uint8_t pad[0x200];
};

#define EMMC_BOOT_NAME		"EMMC_BOOT"
#define SF_BOOT_NAME		"SF_BOOT"
#define SDMMC_BOOT_NAME		"SDMMC_BOOT"

/* ------------------------------------------------------------------ */
/* 2. BootROM 布局头（文件偏移 0x200）                                 */
/* ------------------------------------------------------------------ */

#define BRLYT_NAME		"BRLYT"
#define BRLYT_MAGIC		0x42424242

/* 本镜像中紧随 name/version/header_size/total_size 之后有 8 个 20 字节
 * 的 region 槽位，前 4 个有效、后 4 个全零。U-Boot 的 brom_layout_header
 * 只描述第一个槽位（magic/type/header_size_2/total_size_2/unused）。 */
#define BRLYT_MAX_REGIONS	8
#define BRLYT_REGION_SIZE	20

struct brom_layout_region {
	uint32_t magic;			/* BRLYT_MAGIC，或 0 表示空槽位 */
	uint32_t type;			/* 低 16 位 = flash 类型，高 16 位 = 镜像序号 */
	uint32_t header_size_2;		/* 镜像在文件中的起始偏移 */
	uint32_t total_size_2;		/* 镜像在文件中的结束偏移（非长度） */
	uint32_t unused;		/* 疑似标志位，本镜像见 0x1 / 0x80000001 / 0x80000000 */
};

struct brom_layout_header {
	char name[8];
	uint32_t version;
	uint32_t header_size;		/* 头部总长：0x800，即第一个镜像的起始偏移 */
	uint32_t total_size;		/* 0xe2a0，即最后一个 BRLYT 镜像的结束偏移 */
	struct brom_layout_region region[BRLYT_MAX_REGIONS];
};

#define BRLYT_REGION_INDEX(t)	((uint16_t)((t) >> 16))
#define BRLYT_REGION_FLASH(t)	((uint16_t)((t) & 0xffffu))

/* 高 16 位为 0xfffe 的槽位不指向文件内的镜像：
 * 本镜像 region[0]/region[1] 的 header_size_2 分别是 0x70008004 /
 * 0x7000d3fc（内部 SRAM 地址，而非文件偏移），语义未确认，按保留项处理。 */
#define BRLYT_INDEX_RESERVED	0xfffeu

/* ------------------------------------------------------------------ */
/* 3. GFH（GFH = "MMM" 链式头，位于每个镜像起始处）                    */
/* ------------------------------------------------------------------ */

#define GFH_HEADER_MAGIC	"MMM"

struct gfh_common_header {
	uint8_t magic[3];
	uint8_t version;
	uint16_t size;
	uint16_t type;
};

enum gfh_type {
	GFH_TYPE_FILE_INFO	= 0,
	GFH_TYPE_BL_INFO	= 1,
	GFH_TYPE_ANTI_CLONE	= 2,
	GFH_TYPE_BL_SEC_KEY	= 3,
	GFH_TYPE_BROM_CFG	= 7,
	GFH_TYPE_BROM_SEC_CFG	= 8,

	/* 以下为本镜像中出现、但 U-Boot mtk_image.h 未定义的 MTK 私有类型 */
	GFH_TYPE_EMI_INFO	= 0x101,	/* "EMI_INF"：外部存储器(EMI)初始化参数 */
	GFH_TYPE_MAUI_102	= 0x102,	/* 含义未知 */
	GFH_TYPE_MAUI_103	= 0x103,	/* 含义未知 */
	GFH_TYPE_PROJECT_INFO	= 0x200,	/* 工程名 / 版本字符串 */
	GFH_TYPE_MAUI_207	= 0x207,	/* 含义未知 */
	GFH_TYPE_MAUI_209	= 0x209,	/* 含义未知 */
	GFH_TYPE_MAUI_20E	= 0x20e,	/* 含义未知 */
};

struct gfh_file_info {
	struct gfh_common_header gfh;
	char name[12];
	uint32_t unused;
	uint16_t file_type;
	uint8_t flash_type;
	uint8_t sig_type;
	uint32_t load_addr;
	uint32_t total_size;
	uint32_t max_size;
	uint32_t hdr_size;
	uint32_t sig_size;
	uint32_t jump_offset;
	uint32_t processed;
};

#define GFH_FILE_INFO_NAME	"FILE_INFO"

struct gfh_bl_info {
	struct gfh_common_header gfh;
	uint32_t attr;
};

struct gfh_brom_cfg {
	struct gfh_common_header gfh;
	uint32_t cfg_bits;
	uint32_t usbdl_by_auto_detect_timeout_ms;
	uint8_t unused[0x45];
	uint8_t jump_bl_arm64;
	uint8_t unused2[2];
	uint32_t usbdl_by_kcol0_timeout_ms;
	uint32_t usbdl_by_flag_timeout_ms;
	uint32_t pad;
};

#define GFH_BROM_CFG_USBDL_BY_AUTO_DETECT_TIMEOUT_EN	0x02
#define GFH_BROM_CFG_USBDL_AUTO_DETECT_DIS		0x10
#define GFH_BROM_CFG_USBDL_BY_KCOL0_TIMEOUT_EN		0x80
#define GFH_BROM_CFG_USBDL_BY_FLAG_TIMEOUT_EN		0x100
#define GFH_BROM_CFG_JUMP_BL_ARM64_EN			0x1000
#define GFH_BROM_CFG_JUMP_BL_ARM64			0x64

struct gfh_bl_sec_key {
	struct gfh_common_header gfh;
	uint8_t pad[0x20c];
};

struct gfh_anti_clone {
	struct gfh_common_header gfh;
	uint8_t ac_b2k;
	uint8_t ac_b2c;
	uint16_t pad;
	uint32_t ac_offset;
	uint32_t ac_len;
};

struct gfh_brom_sec_cfg {
	struct gfh_common_header gfh;
	uint32_t cfg_bits;
	char customer_name[0x20];
	uint32_t pad;
};

/* 本镜像观测到的 file_type */
enum gfh_file_type {
	GFH_FILE_TYPE_BL	= 0x001,	/* 一级 Bootloader            */
	GFH_FILE_TYPE_EXT_BL	= 0x002,	/* 二级 / 扩展 Bootloader     */
	GFH_FILE_TYPE_MAUI	= 0x100,	/* MAUI 主固件                */
	GFH_FILE_TYPE_MAUI_108	= 0x108,	/* 主固件之后的第二段镜像     */
};

#define GFH_SIG_TYPE_NONE	0
#define GFH_SIG_TYPE_SHA256	1

/* ------------------------------------------------------------------ */
/* 4. 解析结果                                                         */
/* ------------------------------------------------------------------ */

#define MTK_IMAGE_MAX_IMAGES	16
#define MTK_IMAGE_MAX_GFH	16

struct mtk_gfh_entry {
	uint32_t offset;		/* 该 GFH 在文件中的偏移 */
	uint16_t type;
	uint16_t size;
	uint8_t version;
	char name[13];			/* GFH 体中前 12 字节（可打印时） */
};

struct mtk_image {
	uint32_t offset;		/* 镜像（= FILE_INFO）在文件中的偏移 */
	uint32_t total_size;		/* 含 GFH 头的总长度 */
	uint32_t end;			/* offset + total_size */
	uint32_t hdr_size;		/* GFH 头总长度 */
	uint32_t sig_size;
	uint32_t jump_offset;
	uint32_t load_addr;
	uint32_t entry_point;		/* load_addr + jump_offset */
	uint16_t file_type;
	uint8_t flash_type;
	uint8_t sig_type;
	uint32_t processed;
	char name[13];

	struct mtk_gfh_entry gfh[MTK_IMAGE_MAX_GFH];
	unsigned int gfh_count;
};

struct mtk_boot_image {
	const uint8_t *data;		/* 不拥有，指向调用者的缓冲区 */
	size_t size;

	union gen_boot_header boot;
	struct brom_layout_header brlyt;
	unsigned int brlyt_region_count;/* magic 有效的槽位数 */

	char project[64];		/* GFH_TYPE_PROJECT_INFO 里的工程名 */

	struct mtk_image image[MTK_IMAGE_MAX_IMAGES];
	unsigned int image_count;
};

enum mtk_image_err {
	MTK_IMG_OK		= 0,
	MTK_IMG_ERR_TOO_SMALL	= -1,	/* 缓冲区太小，不是合法镜像 */
	MTK_IMG_ERR_BAD_BOOT	= -2,	/* 设备头 name 不是已知的 *_BOOT */
	MTK_IMG_ERR_BAD_BRLYT	= -3,	/* BRLYT 头缺失 / magic 不对 */
	MTK_IMG_ERR_NOMEM	= -4,
	MTK_IMG_ERR_IO		= -5,
};

/* 把整个文件读进一块 malloc 出来的缓冲区；*buf 需由调用者 free()。 */
int mtk_boot_image_load_file(const char *path, uint8_t **buf, size_t *len);

/* 解析内存中的镜像。out->data 会指向 buf，不复制数据。 */
int mtk_boot_image_parse(const void *buf, size_t len, struct mtk_boot_image *out);

/* 按 file_type 查找镜像，找不到返回 NULL。 */
const struct mtk_image *mtk_boot_image_find(const struct mtk_boot_image *img,
					    uint16_t file_type);

/* 取镜像正文（跳过 GFH 头）；len 可为 NULL。 */
const uint8_t *mtk_image_payload(const struct mtk_boot_image *img,
				 const struct mtk_image *im, size_t *len);

const char *mtk_file_type_name(uint16_t file_type);
const char *mtk_gfh_type_name(uint16_t type);

/* 打印完整结构（走 rxi-log 的 log_info）。 */
void mtk_boot_image_dump(const struct mtk_boot_image *img);

#endif /* _MTK_BOOT_IMAGE_H */
