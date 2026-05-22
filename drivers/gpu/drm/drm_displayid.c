// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */

/*
 * 文件名: drm_displayid.c
 *
 * 中文描述: DisplayID 数据解析
 *
 * DisplayID（Display Identification Data，显示器标识数据）是 VESA 标准定义
 * 的一种描述显示器能力的扩展数据结构。它通常作为 EDID（Extended Display
 * Identification Data）的扩展块存在，提供比传统 EDID 更丰富的显示能力描述，
 * 如更多的时序信息、显示器颜色特性、接口带宽等。
 *
 * 本文件实现了 DisplayID 数据块的迭代解析机制：
 *   1. 遍历 EDID 中所有的 DisplayID 扩展块
 *   2. 对每个 DisplayID 数据块进行校验和验证
 *   3. 支持逐块迭代访问，方便调用方处理各个 DisplayID 数据块
 *   4. 提供版本号和主要用途信息的查询
 *
 * 同时包含针对特定显示器（如 CSO MNE007ZA1-5）的 quirks 处理，
 * 用于绕过其 DisplayID 校验和不正确的问题。
 */

#include <drm/drm_edid.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"
#include "drm_displayid_internal.h"

enum {
	QUIRK_IGNORE_CHECKSUM,
};

struct displayid_quirk {
	const struct drm_edid_ident ident;
	u8 quirks;
};

static const struct displayid_quirk quirks[] = {
	{
		.ident = DRM_EDID_IDENT_INIT('C', 'S', 'O', 5142, "MNE007ZA1-5"),
		.quirks = BIT(QUIRK_IGNORE_CHECKSUM),
	},
};

static u8 get_quirks(const struct drm_edid *drm_edid)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(quirks); i++) {
		if (drm_edid_match(drm_edid, &quirks[i].ident))
			return quirks[i].quirks;
	}

	return 0;
}

static const struct displayid_header *
displayid_get_header(const u8 *displayid, int length, int index)
{
	const struct displayid_header *base;

	if (sizeof(*base) > length - index)
		return ERR_PTR(-EINVAL);

	base = (const struct displayid_header *)&displayid[index];

	return base;
}

/*
 * validate_displayid - 验证 DisplayID 数据块的有效性
 * @displayid: DisplayID 数据缓冲区
 * @length: 数据缓冲区总长度
 * @idx: 起始解析位置
 * @ignore_checksum: 是否忽略校验和错误
 *
 * 验证指定位置的 DisplayID 块是否有效。检查内容包括：
 *   1. DisplayID 头部长度是否在缓冲区范围内
 *   2. 整个数据块（头部 + 数据 + 校验和）是否在缓冲区范围内
 *   3. 校验和是否正确（所有字节加起来应为 0）
 *
 * 对于已知有校验和问题的显示器（如 CSO MNE007ZA1-5），可以通过
 * ignore_checksum 参数绕过校验和检查。
 *
 * 返回：有效的 DisplayID 头部指针，或 ERR_PTR 错误码
 */
static const struct displayid_header *
validate_displayid(const u8 *displayid, int length, int idx, bool ignore_checksum)
{
	int i, dispid_length;
	u8 csum = 0;
	const struct displayid_header *base;

	base = displayid_get_header(displayid, length, idx);
	if (IS_ERR(base))
		return base;

	/* +1 for DispID checksum */
	dispid_length = sizeof(*base) + base->bytes + 1;
	if (dispid_length > length - idx)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < dispid_length; i++)
		csum += displayid[idx + i];
	if (csum) {
		DRM_NOTE("DisplayID checksum invalid, remainder is %d%s\n", csum,
			 ignore_checksum ? " (ignoring)" : "");

		if (!ignore_checksum)
			return ERR_PTR(-EINVAL);
	}

	return base;
}

/*
 * find_next_displayid_extension - 查找下一个 DisplayID 扩展块
 * @iter: DisplayID 迭代器
 *
 * 在 EDID 数据中查找下一个 DisplayID 扩展块。扩展块的类型由
 * DISPLAYID_EXT 标识。找到后验证其有效性，并更新迭代器的位置
 * 和长度信息。
 *
 * 返回：DisplayID 扩展块数据指针，若无更多扩展块则返回 NULL
 */
static const u8 *find_next_displayid_extension(struct displayid_iter *iter)
{
	const struct displayid_header *base;
	const u8 *displayid;
	bool ignore_checksum = iter->quirks & BIT(QUIRK_IGNORE_CHECKSUM);

	displayid = drm_edid_find_extension(iter->drm_edid, DISPLAYID_EXT, &iter->ext_index);
	if (!displayid)
		return NULL;

	/* EDID extensions block checksum isn't for us */
	iter->length = EDID_LENGTH - 1;
	iter->idx = 1;

	base = validate_displayid(displayid, iter->length, iter->idx, ignore_checksum);
	if (IS_ERR(base))
		return NULL;

	iter->length = iter->idx + sizeof(*base) + base->bytes;

	return displayid;
}

/*
 * displayid_iter_edid_begin - 初始化 DisplayID 迭代器
 * @drm_edid: DRM EDID 对象
 * @iter: 要初始化的 DisplayID 迭代器
 *
 * 初始化一个 DisplayID 迭代器，准备遍历给定 EDID 中所有
 * DisplayID 数据块。同时检查该 EDID 是否有已知的 quirks。
 */
void displayid_iter_edid_begin(const struct drm_edid *drm_edid,
			       struct displayid_iter *iter)
{
	memset(iter, 0, sizeof(*iter));

	iter->drm_edid = drm_edid;
	iter->quirks = get_quirks(drm_edid);
}

static const struct displayid_block *
displayid_iter_block(const struct displayid_iter *iter)
{
	const struct displayid_block *block;

	if (!iter->section)
		return NULL;

	block = (const struct displayid_block *)&iter->section[iter->idx];

	if (iter->idx + sizeof(*block) <= iter->length &&
	    iter->idx + sizeof(*block) + block->num_bytes <= iter->length)
		return block;

	return NULL;
}

/*
 * __displayid_iter_next - 获取迭代器中的下一个 DisplayID 数据块
 * @iter: DisplayID 迭代器
 *
 * 返回迭代器当前位置的下一个 DisplayID 数据块。如果当前 section
 * 中还有未遍历的块，直接返回下一个块；否则查找下一个 DisplayID
 * 扩展块并从中返回第一个数据块。
 *
 * 如果是第一个 section（基础 section），还会从中提取 DisplayID
 * 的版本号和主要用途信息。
 *
 * 返回：下一个 DisplayID 数据块指针，遍历完毕返回 NULL
 */
const struct displayid_block *
__displayid_iter_next(struct displayid_iter *iter)
{
	const struct displayid_block *block;

	if (!iter->drm_edid)
		return NULL;

	if (iter->section) {
		/* current block should always be valid */
		block = displayid_iter_block(iter);
		if (WARN_ON(!block)) {
			iter->section = NULL;
			iter->drm_edid = NULL;
			return NULL;
		}

		/* next block in section */
		iter->idx += sizeof(*block) + block->num_bytes;

		block = displayid_iter_block(iter);
		if (block)
			return block;
	}

	for (;;) {
		/* The first section we encounter is the base section */
		bool base_section = !iter->section;

		iter->section = find_next_displayid_extension(iter);
		if (!iter->section) {
			iter->drm_edid = NULL;
			return NULL;
		}

		/* Save the structure version and primary use case. */
		if (base_section) {
			const struct displayid_header *base;

			base = displayid_get_header(iter->section, iter->length,
						    iter->idx);
			if (!IS_ERR(base)) {
				iter->version = base->rev;
				iter->primary_use = base->prod_id;
			}
		}

		iter->idx += sizeof(struct displayid_header);

		block = displayid_iter_block(iter);
		if (block)
			return block;
	}
}

/*
 * displayid_iter_end - 结束 DisplayID 迭代并清理资源
 * @iter: 要清理的 DisplayID 迭代器
 *
 * 结束 DisplayID 遍历，清空迭代器内容。
 */
void displayid_iter_end(struct displayid_iter *iter)
{
	memset(iter, 0, sizeof(*iter));
}

/*
 * displayid_version - 获取 DisplayID 结构版本号
 * @iter: DisplayID 迭代器
 *
 * 返回：DisplayID 基础 section 中的结构版本号/修订号
 */
u8 displayid_version(const struct displayid_iter *iter)
{
	return iter->version;
}

/*
 * displayid_primary_use - 获取 DisplayID 主要用途
 * @iter: DisplayID 迭代器
 *
 * 返回 DisplayID 的主要用途（版本 2.0+）或产品类型标识符
 * （版本 1.0-1.3），来自基础 section。
 *
 * 返回：主要用途码
 */
u8 displayid_primary_use(const struct displayid_iter *iter)
{
	return iter->primary_use;
}
