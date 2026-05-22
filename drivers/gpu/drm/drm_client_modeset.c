// SPDX-License-Identifier: MIT
/*
 * Copyright 2018 Noralf Trønnes
 * Copyright (c) 2006-2009 Red Hat Inc.
 * Copyright (c) 2006-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 * Copyright (c) 2007 Dave Airlie <airlied@linux.ie>
 */

/*
 * DRM 客户端模式设置（Client Modesetting）
 *
 * 本文件实现了 DRM 客户端的模式设置功能，包括显示探测、CRTC 配置、
 * 模式提交和电源管理等操作。这些功能是 fbdev 模拟和其他内核 DRM 客户端
 * 的核心基础。
 *
 * 主要功能：
 *   - drm_client_modeset_probe：探测显示设备并建立初始管线配置
 *   - drm_client_modeset_commit/commit_locked：提交模式设置配置
 *   - drm_client_modeset_check：检查模式设置配置的有效性
 *   - drm_client_modeset_dpms：显示电源管理控制
 *   - drm_client_modeset_wait_for_vblank：等待 VBLANK 同步
 *   - drm_client_rotation：检查并配置面板旋转
 *
 * 探测算法逻辑：
 *   1. 尝试使用固件配置（BIOS/UEFI 预置的显示配置）
 *   2. 如果无法使用固件配置，尝试在所有连接器上进行克隆显示
 *   3. 最后，为每个连接器选择合适的首选模式
 *
 * 模式提交支持原子（atomic）和传统（legacy）两种路径，
 * 根据驱动是否使用原子模式设置自动选择。
 */

#include "drm/drm_modeset_lock.h"

#include <linux/export.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string_helpers.h>

#include <drm/drm_atomic.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_print.h>

#include "drm_crtc_internal.h"
#include "drm_internal.h"

#define DRM_CLIENT_MAX_CLONED_CONNECTORS	8

struct drm_client_offset {
	int x, y;
};

int drm_client_modeset_create(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	unsigned int num_crtc = dev->mode_config.num_crtc;
	unsigned int max_connector_count = 1;
	struct drm_mode_set *modeset;
	struct drm_crtc *crtc;
	int i = 0;

	/* Add terminating zero entry to enable index less iteration */
	client->modesets = kzalloc_objs(*client->modesets, num_crtc + 1);
	if (!client->modesets)
		return -ENOMEM;

	mutex_init(&client->modeset_mutex);

	drm_for_each_crtc(crtc, dev)
		client->modesets[i++].crtc = crtc;

	/* Cloning is only supported in the single crtc case. */
	if (num_crtc == 1)
		max_connector_count = DRM_CLIENT_MAX_CLONED_CONNECTORS;

	for (modeset = client->modesets; modeset->crtc; modeset++) {
		modeset->connectors = kzalloc_objs(*modeset->connectors,
						   max_connector_count);
		if (!modeset->connectors)
			goto err_free;
	}

	return 0;

err_free:
	drm_client_modeset_free(client);

	return -ENOMEM;
}

static void drm_client_modeset_release(struct drm_client_dev *client)
{
	struct drm_mode_set *modeset;

	drm_client_for_each_modeset(modeset, client) {
		int i;

		drm_mode_destroy(client->dev, modeset->mode);
		modeset->mode = NULL;
		modeset->fb = NULL;

		for (i = 0; i < modeset->num_connectors; i++) {
			drm_connector_put(modeset->connectors[i]);
			modeset->connectors[i] = NULL;
		}
		modeset->num_connectors = 0;
	}
}

void drm_client_modeset_free(struct drm_client_dev *client)
{
	struct drm_mode_set *modeset;

	mutex_lock(&client->modeset_mutex);

	drm_client_modeset_release(client);

	drm_client_for_each_modeset(modeset, client)
		kfree(modeset->connectors);

	mutex_unlock(&client->modeset_mutex);

	mutex_destroy(&client->modeset_mutex);
	kfree(client->modesets);
}

static struct drm_mode_set *
drm_client_find_modeset(struct drm_client_dev *client, struct drm_crtc *crtc)
{
	struct drm_mode_set *modeset;

	drm_client_for_each_modeset(modeset, client)
		if (modeset->crtc == crtc)
			return modeset;

	return NULL;
}

static const struct drm_display_mode *
drm_connector_get_tiled_mode(struct drm_connector *connector)
{
	const struct drm_display_mode *mode;

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->hdisplay == connector->tile_h_size &&
		    mode->vdisplay == connector->tile_v_size)
			return mode;
	}
	return NULL;
}

static const struct drm_display_mode *
drm_connector_fallback_non_tiled_mode(struct drm_connector *connector)
{
	const struct drm_display_mode *mode;

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->hdisplay == connector->tile_h_size &&
		    mode->vdisplay == connector->tile_v_size)
			continue;
		return mode;
	}
	return NULL;
}

static const struct drm_display_mode *
drm_connector_preferred_mode(struct drm_connector *connector, int width, int height)
{
	const struct drm_display_mode *mode;

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->hdisplay > width ||
		    mode->vdisplay > height)
			continue;
		if (mode->type & DRM_MODE_TYPE_PREFERRED)
			return mode;
	}
	return NULL;
}

static const struct drm_display_mode *
drm_connector_first_mode(struct drm_connector *connector)
{
	return list_first_entry_or_null(&connector->modes,
					struct drm_display_mode, head);
}

static const struct drm_display_mode *
drm_connector_pick_cmdline_mode(struct drm_connector *connector)
{
	const struct drm_cmdline_mode *cmdline_mode;
	const struct drm_display_mode *mode;
	bool prefer_non_interlace;

	/*
	 * Find a user-defined mode. If the user gave us a valid
	 * mode on the kernel command line, it will show up in this
	 * list.
	 */

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->type & DRM_MODE_TYPE_USERDEF)
			return mode;
	}

	cmdline_mode = &connector->cmdline_mode;
	if (cmdline_mode->specified == false)
		return NULL;

	/*
	 * Attempt to find a matching mode in the list of modes we
	 * have gotten so far.
	 */

	prefer_non_interlace = !cmdline_mode->interlace;
again:
	list_for_each_entry(mode, &connector->modes, head) {
		/* check width/height */
		if (mode->hdisplay != cmdline_mode->xres ||
		    mode->vdisplay != cmdline_mode->yres)
			continue;

		if (cmdline_mode->refresh_specified) {
			if (drm_mode_vrefresh(mode) != cmdline_mode->refresh)
				continue;
		}

		if (cmdline_mode->interlace) {
			if (!(mode->flags & DRM_MODE_FLAG_INTERLACE))
				continue;
		} else if (prefer_non_interlace) {
			if (mode->flags & DRM_MODE_FLAG_INTERLACE)
				continue;
		}
		return mode;
	}

	if (prefer_non_interlace) {
		prefer_non_interlace = false;
		goto again;
	}

	return NULL;
}

static bool drm_connector_enabled(struct drm_connector *connector, bool strict)
{
	bool enable;

	if (connector->display_info.non_desktop)
		return false;

	if (strict)
		enable = connector->status == connector_status_connected;
	else
		enable = connector->status != connector_status_disconnected;

	return enable;
}

static void drm_client_connectors_enabled(struct drm_connector *connectors[],
					  unsigned int connector_count,
					  bool enabled[])
{
	bool any_enabled = false;
	struct drm_connector *connector;
	int i = 0;

	for (i = 0; i < connector_count; i++) {
		connector = connectors[i];
		enabled[i] = drm_connector_enabled(connector, true);
		drm_dbg_kms(connector->dev, "[CONNECTOR:%d:%s] enabled? %s\n",
			    connector->base.id, connector->name,
			    connector->display_info.non_desktop ?
			    "non desktop" : str_yes_no(enabled[i]));

		any_enabled |= enabled[i];
	}

	if (any_enabled)
		return;

	for (i = 0; i < connector_count; i++)
		enabled[i] = drm_connector_enabled(connectors[i], false);
}

static void mode_replace(struct drm_device *dev,
			 const struct drm_display_mode **dst,
			 const struct drm_display_mode *src)
{
	drm_mode_destroy(dev, (struct drm_display_mode *)*dst);

	*dst = src ? drm_mode_duplicate(dev, src) : NULL;
}

static void modes_destroy(struct drm_device *dev,
			  const struct drm_display_mode *modes[],
			  int count)
{
	int i;

	for (i = 0; i < count; i++)
		mode_replace(dev, &modes[i], NULL);
}

static bool drm_client_target_cloned(struct drm_device *dev,
				     struct drm_connector *connectors[],
				     unsigned int connector_count,
				     const struct drm_display_mode *modes[],
				     struct drm_client_offset offsets[],
				     bool enabled[], int width, int height)
{
	int count, i;
	bool can_clone = false;
	struct drm_display_mode *dmt_mode;

	/* only contemplate cloning in the single crtc case */
	if (dev->mode_config.num_crtc > 1)
		return false;

	count = 0;
	for (i = 0; i < connector_count; i++) {
		if (enabled[i])
			count++;
	}

	/* only contemplate cloning if more than one connector is enabled */
	if (count <= 1)
		return false;

	/* check the command line or if nothing common pick 1024x768 */
	can_clone = true;
	for (i = 0; i < connector_count; i++) {
		int j;

		if (!enabled[i])
			continue;

		mode_replace(dev, &modes[i],
			     drm_connector_pick_cmdline_mode(connectors[i]));
		if (!modes[i]) {
			can_clone = false;
			break;
		}
		for (j = 0; j < i; j++) {
			if (!enabled[j])
				continue;
			if (!drm_mode_match(modes[j], modes[i],
					    DRM_MODE_MATCH_TIMINGS |
					    DRM_MODE_MATCH_CLOCK |
					    DRM_MODE_MATCH_FLAGS |
					    DRM_MODE_MATCH_3D_FLAGS))
				can_clone = false;
		}
	}

	if (can_clone) {
		drm_dbg_kms(dev, "can clone using command line\n");
		return true;
	}

	/* try and find a 1024x768 mode on each connector */
	can_clone = true;
	dmt_mode = drm_mode_find_dmt(dev, 1024, 768, 60, false);

	if (!dmt_mode)
		goto fail;

	for (i = 0; i < connector_count; i++) {
		const struct drm_display_mode *mode;

		if (!enabled[i])
			continue;

		list_for_each_entry(mode, &connectors[i]->modes, head) {
			if (drm_mode_match(mode, dmt_mode,
					   DRM_MODE_MATCH_TIMINGS |
					   DRM_MODE_MATCH_CLOCK |
					   DRM_MODE_MATCH_FLAGS |
					   DRM_MODE_MATCH_3D_FLAGS))
				mode_replace(dev, &modes[i], mode);
		}
		if (!modes[i])
			can_clone = false;
	}
	drm_mode_destroy(dev, dmt_mode);

	if (can_clone) {
		drm_dbg_kms(dev, "can clone using 1024x768\n");
		return true;
	}
fail:
	drm_info(dev, "kms: can't enable cloning when we probably wanted to.\n");
	return false;
}

static int drm_client_get_tile_offsets(struct drm_device *dev,
				       struct drm_connector *connectors[],
				       unsigned int connector_count,
				       const struct drm_display_mode *modes[],
				       struct drm_client_offset offsets[],
				       int idx,
				       int h_idx, int v_idx)
{
	int i;
	int hoffset = 0, voffset = 0;

	for (i = 0; i < connector_count; i++) {
		struct drm_connector *connector = connectors[i];

		if (!connector->has_tile)
			continue;

		if (!modes[i] && (h_idx || v_idx)) {
			drm_dbg_kms(dev,
				    "[CONNECTOR:%d:%s] no modes for connector tiled %d\n",
				    connector->base.id, connector->name, i);
			continue;
		}
		if (connector->tile_h_loc < h_idx)
			hoffset += modes[i]->hdisplay;

		if (connector->tile_v_loc < v_idx)
			voffset += modes[i]->vdisplay;
	}
	offsets[idx].x = hoffset;
	offsets[idx].y = voffset;
	drm_dbg_kms(dev, "returned %d %d for %d %d\n", hoffset, voffset, h_idx, v_idx);
	return 0;
}

static bool drm_client_target_preferred(struct drm_device *dev,
					struct drm_connector *connectors[],
					unsigned int connector_count,
					const struct drm_display_mode *modes[],
					struct drm_client_offset offsets[],
					bool enabled[], int width, int height)
{
	const u64 mask = BIT_ULL(connector_count) - 1;
	u64 conn_configured = 0;
	int tile_pass = 0;
	int num_tiled_conns = 0;
	int i;

	for (i = 0; i < connector_count; i++) {
		if (connectors[i]->has_tile &&
		    connectors[i]->status == connector_status_connected)
			num_tiled_conns++;
	}

retry:
	for (i = 0; i < connector_count; i++) {
		struct drm_connector *connector = connectors[i];
		const char *mode_type;


		if (conn_configured & BIT_ULL(i))
			continue;

		if (enabled[i] == false) {
			conn_configured |= BIT_ULL(i);
			continue;
		}

		/* first pass over all the untiled connectors */
		if (tile_pass == 0 && connector->has_tile)
			continue;

		if (tile_pass == 1) {
			if (connector->tile_h_loc != 0 ||
			    connector->tile_v_loc != 0)
				continue;

		} else {
			if (connector->tile_h_loc != tile_pass - 1 &&
			    connector->tile_v_loc != tile_pass - 1)
			/* if this tile_pass doesn't cover any of the tiles - keep going */
				continue;

			/*
			 * find the tile offsets for this pass - need to find
			 * all tiles left and above
			 */
			drm_client_get_tile_offsets(dev, connectors, connector_count,
						    modes, offsets, i,
						    connector->tile_h_loc, connector->tile_v_loc);
		}

		mode_type = "cmdline";
		mode_replace(dev, &modes[i],
			     drm_connector_pick_cmdline_mode(connector));

		if (!modes[i]) {
			mode_type = "preferred";
			mode_replace(dev, &modes[i],
				     drm_connector_preferred_mode(connector, width, height));
		}

		if (!modes[i]) {
			mode_type = "first";
			mode_replace(dev, &modes[i],
				     drm_connector_first_mode(connector));
		}

		/*
		 * In case of tiled mode if all tiles not present fallback to
		 * first available non tiled mode.
		 * After all tiles are present, try to find the tiled mode
		 * for all and if tiled mode not present due to fbcon size
		 * limitations, use first non tiled mode only for
		 * tile 0,0 and set to no mode for all other tiles.
		 */
		if (connector->has_tile) {
			if (num_tiled_conns <
			    connector->num_h_tile * connector->num_v_tile ||
			    (connector->tile_h_loc == 0 &&
			     connector->tile_v_loc == 0 &&
			     !drm_connector_get_tiled_mode(connector))) {
				mode_type = "non tiled";
				mode_replace(dev, &modes[i],
					     drm_connector_fallback_non_tiled_mode(connector));
			} else {
				mode_type = "tiled";
				mode_replace(dev, &modes[i],
					     drm_connector_get_tiled_mode(connector));
			}
		}

		if (modes[i])
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] found %s mode: %s\n",
				    connector->base.id, connector->name,
				    mode_type, modes[i]->name);
		else
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] no mode found\n",
				    connector->base.id, connector->name);

		conn_configured |= BIT_ULL(i);
	}

	if ((conn_configured & mask) != mask) {
		tile_pass++;
		goto retry;
	}
	return true;
}

static bool connector_has_possible_crtc(struct drm_connector *connector,
					struct drm_crtc *crtc)
{
	struct drm_encoder *encoder;

	drm_connector_for_each_possible_encoder(connector, encoder) {
		if (encoder->possible_crtcs & drm_crtc_mask(crtc))
			return true;
	}

	return false;
}

static int drm_client_pick_crtcs(struct drm_client_dev *client,
				 struct drm_connector *connectors[],
				 unsigned int connector_count,
				 struct drm_crtc *best_crtcs[],
				 const struct drm_display_mode *modes[],
				 int n, int width, int height)
{
	struct drm_device *dev = client->dev;
	struct drm_connector *connector;
	int my_score, best_score, score;
	struct drm_crtc **crtcs;
	struct drm_mode_set *modeset;

	if (n == connector_count)
		return 0;

	connector = connectors[n];

	best_crtcs[n] = NULL;
	best_score = drm_client_pick_crtcs(client, connectors, connector_count,
					   best_crtcs, modes, n + 1, width, height);
	if (modes[n] == NULL)
		return best_score;

	crtcs = kzalloc_objs(*crtcs, connector_count);
	if (!crtcs)
		return best_score;

	my_score = 1;
	if (connector->status == connector_status_connected)
		my_score++;
	if (connector->cmdline_mode.specified)
		my_score++;
	if (drm_connector_preferred_mode(connector, width, height))
		my_score++;

	/*
	 * select a crtc for this connector and then attempt to configure
	 * remaining connectors
	 */
	drm_client_for_each_modeset(modeset, client) {
		struct drm_crtc *crtc = modeset->crtc;
		int o;

		if (!connector_has_possible_crtc(connector, crtc))
			continue;

		for (o = 0; o < n; o++)
			if (best_crtcs[o] == crtc)
				break;

		if (o < n) {
			/* ignore cloning unless only a single crtc */
			if (dev->mode_config.num_crtc > 1)
				continue;

			if (!drm_mode_equal(modes[o], modes[n]))
				continue;
		}

		crtcs[n] = crtc;
		memcpy(crtcs, best_crtcs, n * sizeof(*crtcs));
		score = my_score + drm_client_pick_crtcs(client, connectors, connector_count,
							 crtcs, modes, n + 1, width, height);
		if (score > best_score) {
			best_score = score;
			memcpy(best_crtcs, crtcs, connector_count * sizeof(*crtcs));
		}
	}

	kfree(crtcs);
	return best_score;
}

/* Try to read the BIOS display configuration and use it for the initial config */
static bool drm_client_firmware_config(struct drm_client_dev *client,
				       struct drm_connector *connectors[],
				       unsigned int connector_count,
				       struct drm_crtc *crtcs[],
				       const struct drm_display_mode *modes[],
				       struct drm_client_offset offsets[],
				       bool enabled[], int width, int height)
{
	const int count = min_t(unsigned int, connector_count, BITS_PER_LONG);
	unsigned long conn_configured, conn_seq, mask;
	struct drm_device *dev = client->dev;
	int i;
	bool *save_enabled;
	bool fallback = true, ret = true;
	int num_connectors_enabled = 0;
	int num_connectors_detected = 0;
	int num_tiled_conns = 0;
	struct drm_modeset_acquire_ctx ctx;

	if (!drm_drv_uses_atomic_modeset(dev))
		return false;

	if (drm_WARN_ON(dev, count <= 0))
		return false;

	save_enabled = kzalloc_objs(bool, count);
	if (!save_enabled)
		return false;

	drm_modeset_acquire_init(&ctx, 0);

	while (drm_modeset_lock_all_ctx(dev, &ctx) != 0)
		drm_modeset_backoff(&ctx);

	memcpy(save_enabled, enabled, count);
	mask = GENMASK(count - 1, 0);
	conn_configured = 0;
	for (i = 0; i < count; i++) {
		if (connectors[i]->has_tile &&
		    connectors[i]->status == connector_status_connected)
			num_tiled_conns++;
	}
retry:
	conn_seq = conn_configured;
	for (i = 0; i < count; i++) {
		struct drm_connector *connector = connectors[i];
		struct drm_encoder *encoder;
		struct drm_crtc *crtc;
		const char *mode_type;
		int j;

		if (conn_configured & BIT(i))
			continue;

		if (conn_seq == 0 && !connector->has_tile)
			continue;

		if (connector->status == connector_status_connected)
			num_connectors_detected++;

		if (!enabled[i]) {
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] not enabled, skipping\n",
				    connector->base.id, connector->name);
			conn_configured |= BIT(i);
			continue;
		}

		if (connector->force == DRM_FORCE_OFF) {
			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] disabled by user, skipping\n",
				    connector->base.id, connector->name);
			enabled[i] = false;
			continue;
		}

		encoder = connector->state->best_encoder;
		if (!encoder || drm_WARN_ON(dev, !connector->state->crtc)) {
			if (connector->force > DRM_FORCE_OFF)
				goto bail;

			drm_dbg_kms(dev, "[CONNECTOR:%d:%s] has no encoder or crtc, skipping\n",
				    connector->base.id, connector->name);
			enabled[i] = false;
			conn_configured |= BIT(i);
			continue;
		}

		num_connectors_enabled++;

		crtc = connector->state->crtc;

		/*
		 * Make sure we're not trying to drive multiple connectors
		 * with a single CRTC, since our cloning support may not
		 * match the BIOS.
		 */
		for (j = 0; j < count; j++) {
			if (crtcs[j] == crtc) {
				drm_dbg_kms(dev, "[CONNECTOR:%d:%s] fallback: cloned configuration\n",
					    connector->base.id, connector->name);
				goto bail;
			}
		}

		mode_type = "cmdline";
		mode_replace(dev, &modes[i],
			     drm_connector_pick_cmdline_mode(connector));

		if (!modes[i]) {
			mode_type = "preferred";
			mode_replace(dev, &modes[i],
				     drm_connector_preferred_mode(connector, width, height));
		}

		if (!modes[i]) {
			mode_type = "first";
			mode_replace(dev, &modes[i],
				     drm_connector_first_mode(connector));
		}

		/* last resort: use current mode */
		if (!modes[i]) {
			mode_type = "current";
			mode_replace(dev, &modes[i],
				     &crtc->state->mode);
		}

		/*
		 * In case of tiled modes, if all tiles are not present
		 * then fallback to a non tiled mode.
		 */
		if (connector->has_tile &&
		    num_tiled_conns < connector->num_h_tile * connector->num_v_tile) {
			mode_type = "non tiled";
			mode_replace(dev, &modes[i],
				     drm_connector_fallback_non_tiled_mode(connector));
		}
		crtcs[i] = crtc;

		drm_dbg_kms(dev, "[CONNECTOR::%d:%s] on [CRTC:%d:%s] using %s mode: %s\n",
			    connector->base.id, connector->name,
			    crtc->base.id, crtc->name,
			    mode_type, modes[i]->name);

		fallback = false;
		conn_configured |= BIT(i);
	}

	if ((conn_configured & mask) != mask && conn_configured != conn_seq)
		goto retry;

	for (i = 0; i < count; i++) {
		struct drm_connector *connector = connectors[i];

		if (connector->has_tile)
			drm_client_get_tile_offsets(dev, connectors, connector_count,
						    modes, offsets, i,
						    connector->tile_h_loc, connector->tile_v_loc);
	}

	/*
	 * If the BIOS didn't enable everything it could, fall back to have the
	 * same user experiencing of lighting up as much as possible like the
	 * fbdev helper library.
	 */
	if (num_connectors_enabled != num_connectors_detected &&
	    num_connectors_enabled < dev->mode_config.num_crtc) {
		drm_dbg_kms(dev, "fallback: Not all outputs enabled\n");
		drm_dbg_kms(dev, "Enabled: %i, detected: %i\n",
			    num_connectors_enabled, num_connectors_detected);
		fallback = true;
	}

	if (fallback) {
bail:
		drm_dbg_kms(dev, "Not using firmware configuration\n");
		memcpy(enabled, save_enabled, count);
		ret = false;
	}

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	kfree(save_enabled);
	return ret;
}

/**
 * drm_client_modeset_probe() - 探测显示设备
 * @client: DRM 客户端
 * @width: 最大显示模式宽度（可选，0 表示使用硬件最大宽度）
 * @height: 最大显示模式高度（可选，0 表示使用硬件最大高度）
 *
 * 为已启用的连接器建立显示管线，并将配置存储到客户端的 modeset 数组中。
 * 此函数是客户端显示配置的核心，执行以下步骤：
 *
 *   1. 收集所有连接器并获取其支持的显示模式
 *   2. 尝试使用固件（BIOS/UEFI）预置的配置
 *   3. 如果固件配置不可用，尝试克隆配置
 *   4. 为每个连接器选择合适的显示模式
 *   5. 为每个模式选择最佳的 CRTC
 *   6. 将最终的配置写入客户端的 modeset 数组
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_modeset_probe(struct drm_client_dev *client, unsigned int width, unsigned int height)
{
	struct drm_connector *connector, **connectors = NULL;
	struct drm_connector_list_iter conn_iter;
	struct drm_device *dev = client->dev;
	unsigned int total_modes_count = 0;
	struct drm_client_offset *offsets;
	unsigned int connector_count = 0;
	const struct drm_display_mode **modes;
	struct drm_crtc **crtcs;
	int i, ret = 0;
	bool *enabled;

	drm_dbg_kms(dev, "\n");

	if (!width)
		width = dev->mode_config.max_width;
	if (!height)
		height = dev->mode_config.max_height;

	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_client_for_each_connector_iter(connector, &conn_iter) {
		struct drm_connector **tmp;

		tmp = krealloc(connectors, (connector_count + 1) * sizeof(*connectors), GFP_KERNEL);
		if (!tmp) {
			ret = -ENOMEM;
			goto free_connectors;
		}

		connectors = tmp;
		drm_connector_get(connector);
		connectors[connector_count++] = connector;
	}
	drm_connector_list_iter_end(&conn_iter);

	if (!connector_count)
		return 0;

	crtcs = kzalloc_objs(*crtcs, connector_count);
	modes = kzalloc_objs(*modes, connector_count);
	offsets = kzalloc_objs(*offsets, connector_count);
	enabled = kzalloc_objs(bool, connector_count);
	if (!crtcs || !modes || !enabled || !offsets) {
		ret = -ENOMEM;
		goto out;
	}

	mutex_lock(&client->modeset_mutex);

	mutex_lock(&dev->mode_config.mutex);
	for (i = 0; i < connector_count; i++)
		total_modes_count += connectors[i]->funcs->fill_modes(connectors[i], width, height);
	if (!total_modes_count)
		drm_dbg_kms(dev, "No connectors reported connected with modes\n");
	drm_client_connectors_enabled(connectors, connector_count, enabled);

	if (!drm_client_firmware_config(client, connectors, connector_count, crtcs,
					modes, offsets, enabled, width, height)) {
		modes_destroy(dev, modes, connector_count);
		memset(crtcs, 0, connector_count * sizeof(*crtcs));
		memset(offsets, 0, connector_count * sizeof(*offsets));

		if (!drm_client_target_cloned(dev, connectors, connector_count, modes,
					      offsets, enabled, width, height) &&
		    !drm_client_target_preferred(dev, connectors, connector_count, modes,
						 offsets, enabled, width, height))
			drm_err(dev, "Unable to find initial modes\n");

		drm_dbg_kms(dev, "picking CRTCs for %dx%d config\n",
			    width, height);

		drm_client_pick_crtcs(client, connectors, connector_count,
				      crtcs, modes, 0, width, height);
	}

	mutex_unlock(&dev->mode_config.mutex);

	drm_client_modeset_release(client);

	for (i = 0; i < connector_count; i++) {
		const struct drm_display_mode *mode = modes[i];
		struct drm_crtc *crtc = crtcs[i];
		struct drm_client_offset *offset = &offsets[i];

		if (mode && crtc) {
			struct drm_mode_set *modeset = drm_client_find_modeset(client, crtc);
			struct drm_connector *connector = connectors[i];

			drm_dbg_kms(dev, "[CRTC:%d:%s] desired mode %s set (%d,%d)\n",
				    crtc->base.id, crtc->name,
				    mode->name, offset->x, offset->y);

			if (drm_WARN_ON_ONCE(dev, modeset->num_connectors == DRM_CLIENT_MAX_CLONED_CONNECTORS ||
					     (dev->mode_config.num_crtc > 1 && modeset->num_connectors == 1))) {
				ret = -EINVAL;
				break;
			}

			drm_mode_destroy(dev, modeset->mode);
			modeset->mode = drm_mode_duplicate(dev, mode);
			if (!modeset->mode) {
				ret = -ENOMEM;
				break;
			}

			drm_connector_get(connector);
			modeset->connectors[modeset->num_connectors++] = connector;
			modeset->x = offset->x;
			modeset->y = offset->y;
		}
	}

	mutex_unlock(&client->modeset_mutex);
out:
	kfree(crtcs);
	if (modes)
		modes_destroy(dev, modes, connector_count);
	kfree(modes);
	kfree(offsets);
	kfree(enabled);
free_connectors:
	for (i = 0; i < connector_count; i++)
		drm_connector_put(connectors[i]);
	kfree(connectors);

	return ret;
}
EXPORT_SYMBOL(drm_client_modeset_probe);

/**
 * drm_client_rotation() - 检查初始旋转值
 * @modeset: DRM 模式设置
 * @rotation: 返回的旋转值
 *
 * 检查 @modeset 中的主平面是否可以通过硬件旋转来匹配其连接器
 * 所需要的旋转方向。考虑面板方向（panel orientation，通过
 * &drm_connector.display_info.panel_orientation 获得）和
 * 内核命令行中指定的旋转/反射参数。
 *
 * 面板方向和命令行旋转参数会相加计算最终的旋转值。
 * 反射操作（如 REFLECT_X/Y）则通过 XOR 进行组合。
 *
 * 注意：当前仅支持 0 度和 180 度旋转。90 度/270 度旋转通常
 * 需要特定的帧缓冲 tiling 格式，尚未支持。
 *
 * 返回值：
 * 如果平面可以执行旋转返回 true，否则返回 false。
 */
bool drm_client_rotation(struct drm_mode_set *modeset, unsigned int *rotation)
{
	struct drm_connector *connector = modeset->connectors[0];
	struct drm_plane *plane = modeset->crtc->primary;
	struct drm_cmdline_mode *cmdline;
	u64 valid_mask = 0;
	int i;

	if (!modeset->num_connectors)
		return false;

	switch (connector->display_info.panel_orientation) {
	case DRM_MODE_PANEL_ORIENTATION_BOTTOM_UP:
		*rotation = DRM_MODE_ROTATE_180;
		break;
	case DRM_MODE_PANEL_ORIENTATION_LEFT_UP:
		*rotation = DRM_MODE_ROTATE_90;
		break;
	case DRM_MODE_PANEL_ORIENTATION_RIGHT_UP:
		*rotation = DRM_MODE_ROTATE_270;
		break;
	default:
		*rotation = DRM_MODE_ROTATE_0;
	}

	/**
	 * The panel already defined the default rotation
	 * through its orientation. Whatever has been provided
	 * on the command line needs to be added to that.
	 *
	 * Unfortunately, the rotations are at different bit
	 * indices, so the math to add them up are not as
	 * trivial as they could.
	 *
	 * Reflections on the other hand are pretty trivial to deal with, a
	 * simple XOR between the two handle the addition nicely.
	 */
	cmdline = &connector->cmdline_mode;
	if (cmdline->specified && cmdline->rotation_reflection) {
		unsigned int cmdline_rest, panel_rest;
		unsigned int cmdline_rot, panel_rot;
		unsigned int sum_rot, sum_rest;

		panel_rot = ilog2(*rotation & DRM_MODE_ROTATE_MASK);
		cmdline_rot = ilog2(cmdline->rotation_reflection & DRM_MODE_ROTATE_MASK);
		sum_rot = (panel_rot + cmdline_rot) % 4;

		panel_rest = *rotation & ~DRM_MODE_ROTATE_MASK;
		cmdline_rest = cmdline->rotation_reflection & ~DRM_MODE_ROTATE_MASK;
		sum_rest = panel_rest ^ cmdline_rest;

		*rotation = (1 << sum_rot) | sum_rest;
	}

	/*
	 * TODO: support 90 / 270 degree hardware rotation,
	 * depending on the hardware this may require the framebuffer
	 * to be in a specific tiling format.
	 */
	if (((*rotation & DRM_MODE_ROTATE_MASK) != DRM_MODE_ROTATE_0 &&
	     (*rotation & DRM_MODE_ROTATE_MASK) != DRM_MODE_ROTATE_180) ||
	    !plane->rotation_property)
		return false;

	for (i = 0; i < plane->rotation_property->num_values; i++)
		valid_mask |= (1ULL << plane->rotation_property->values[i]);

	if (!(*rotation & valid_mask))
		return false;

	return true;
}
EXPORT_SYMBOL(drm_client_rotation);

static int drm_client_modeset_commit_atomic(struct drm_client_dev *client, bool active, bool check)
{
	struct drm_device *dev = client->dev;
	struct drm_plane *plane;
	struct drm_atomic_state *state;
	struct drm_modeset_acquire_ctx ctx;
	struct drm_mode_set *mode_set;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_state_alloc(dev);
	if (!state) {
		ret = -ENOMEM;
		goto out_ctx;
	}

	state->acquire_ctx = &ctx;
retry:
	drm_for_each_plane(plane, dev) {
		struct drm_plane_state *plane_state;

		plane_state = drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state)) {
			ret = PTR_ERR(plane_state);
			goto out_state;
		}

		plane_state->rotation = DRM_MODE_ROTATE_0;

		/* disable non-primary: */
		if (plane->type == DRM_PLANE_TYPE_PRIMARY)
			continue;

		ret = __drm_atomic_helper_disable_plane(plane, plane_state);
		if (ret != 0)
			goto out_state;
	}

	drm_client_for_each_modeset(mode_set, client) {
		struct drm_plane *primary = mode_set->crtc->primary;
		unsigned int rotation;

		if (drm_client_rotation(mode_set, &rotation)) {
			struct drm_plane_state *plane_state;

			/* Cannot fail as we've already gotten the plane state above */
			plane_state = drm_atomic_get_new_plane_state(state, primary);
			plane_state->rotation = rotation;
		}

		ret = __drm_atomic_helper_set_config(mode_set, state);
		if (ret != 0)
			goto out_state;

		/*
		 * __drm_atomic_helper_set_config() sets active when a
		 * mode is set, unconditionally clear it if we force DPMS off
		 */
		if (!active) {
			struct drm_crtc *crtc = mode_set->crtc;
			struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

			crtc_state->active = false;
		}
	}

	if (check)
		ret = drm_atomic_check_only(state);
	else
		ret = drm_atomic_commit(state);

out_state:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_put(state);
out_ctx:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;

backoff:
	drm_atomic_state_clear(state);
	drm_modeset_backoff(&ctx);

	goto retry;
}

static int drm_client_modeset_commit_legacy(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	struct drm_mode_set *mode_set;
	struct drm_plane *plane;
	int ret = 0;

	drm_modeset_lock_all(dev);
	drm_for_each_plane(plane, dev) {
		if (plane->type != DRM_PLANE_TYPE_PRIMARY)
			drm_plane_force_disable(plane);

		if (plane->rotation_property)
			drm_mode_plane_set_obj_prop(plane,
						    plane->rotation_property,
						    DRM_MODE_ROTATE_0);
	}

	drm_client_for_each_modeset(mode_set, client) {
		struct drm_crtc *crtc = mode_set->crtc;

		if (crtc->funcs->cursor_set2) {
			ret = crtc->funcs->cursor_set2(crtc, NULL, 0, 0, 0, 0, 0);
			if (ret)
				goto out;
		} else if (crtc->funcs->cursor_set) {
			ret = crtc->funcs->cursor_set(crtc, NULL, 0, 0, 0);
			if (ret)
				goto out;
		}

		ret = drm_mode_set_config_internal(mode_set);
		if (ret)
			goto out;
	}
out:
	drm_modeset_unlock_all(dev);

	return ret;
}

/**
 * drm_client_modeset_check() - 检查模式设置配置
 * @client: DRM 客户端
 *
 * 检查当前模式设置配置是否有效。对于原子驱动，此函数执行
 * drm_atomic_check_only() 来验证配置而不实际提交它。
 * 对于传统（非原子）驱动，直接返回 0（不进行检查）。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_modeset_check(struct drm_client_dev *client)
{
	int ret;

	if (!drm_drv_uses_atomic_modeset(client->dev))
		return 0;

	mutex_lock(&client->modeset_mutex);
	ret = drm_client_modeset_commit_atomic(client, true, true);
	mutex_unlock(&client->modeset_mutex);

	return ret;
}
EXPORT_SYMBOL(drm_client_modeset_check);

/**
 * drm_client_modeset_commit_locked() - 强制提交 CRTC 配置（已加锁版本）
 * @client: DRM 客户端
 *
 * 将模式设置配置提交到 CRTC，不检查当前是否存在 DRM master。
 * 调用者必须已经通过 drm_master_internal_acquire() 获取了内部的
 * DRM master 引用。
 *
 * 此函数会根据驱动类型自动选择提交路径：
 *   - 原子驱动：使用 drm_atomic_commit()
 *   - 传统驱动：使用 drm_mode_set_config_internal()
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_modeset_commit_locked(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	int ret;

	mutex_lock(&client->modeset_mutex);
	if (drm_drv_uses_atomic_modeset(dev))
		ret = drm_client_modeset_commit_atomic(client, true, false);
	else
		ret = drm_client_modeset_commit_legacy(client);
	mutex_unlock(&client->modeset_mutex);

	return ret;
}
EXPORT_SYMBOL(drm_client_modeset_commit_locked);

/**
 * drm_client_modeset_commit() - 提交 CRTC 配置
 * @client: DRM 客户端
 *
 * 将模式设置配置提交到 CRTC。此函数首先尝试获取内部 DRM master 引用，
 * 如果获取失败（存在外部 master），返回 -EBUSY。
 * 获取成功后委托给 drm_client_modeset_commit_locked() 执行实际的提交。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_modeset_commit(struct drm_client_dev *client)
{
	struct drm_device *dev = client->dev;
	int ret;

	if (!drm_master_internal_acquire(dev))
		return -EBUSY;

	ret = drm_client_modeset_commit_locked(client);

	drm_master_internal_release(dev);

	return ret;
}
EXPORT_SYMBOL(drm_client_modeset_commit);

static void drm_client_modeset_dpms_legacy(struct drm_client_dev *client, int dpms_mode)
{
	struct drm_device *dev = client->dev;
	struct drm_connector *connector;
	struct drm_mode_set *modeset;
	struct drm_modeset_acquire_ctx ctx;
	int ret;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);
	drm_client_for_each_modeset(modeset, client) {
		int j;

		if (!modeset->crtc->enabled)
			continue;

		for (j = 0; j < modeset->num_connectors; j++) {
			connector = modeset->connectors[j];
			connector->funcs->dpms(connector, dpms_mode);
			drm_object_property_set_value(&connector->base,
				dev->mode_config.dpms_property, dpms_mode);
		}
	}
	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
}

/**
 * drm_client_modeset_dpms() - 设置 DPMS 模式
 * @client: DRM 客户端
 * @mode: DPMS 模式（DRM_MODE_DPMS_ON/OFF/STANDBY/SUSPEND）
 *
 * 设置显示器的电源管理模式。对于原子驱动，DPMS 模式被简化为开/关
 * （ON 对应 active=true，OFF 对应 active=false），因为原子框架
 * 不支持 STANDBY 和 SUSPEND 之间的细粒度区别。
 * 对于传统驱动，则逐个连接器和编码器地设置 DPMS 状态。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_modeset_dpms(struct drm_client_dev *client, int mode)
{
	struct drm_device *dev = client->dev;
	int ret = 0;

	if (!drm_master_internal_acquire(dev))
		return -EBUSY;

	mutex_lock(&client->modeset_mutex);
	if (drm_drv_uses_atomic_modeset(dev))
		ret = drm_client_modeset_commit_atomic(client, mode == DRM_MODE_DPMS_ON, false);
	else
		drm_client_modeset_dpms_legacy(client, mode);
	mutex_unlock(&client->modeset_mutex);

	drm_master_internal_release(dev);

	return ret;
}
EXPORT_SYMBOL(drm_client_modeset_dpms);

/**
 * drm_client_modeset_wait_for_vblank() - 等待下一次 VBLANK 事件
 * @client: DRM 客户端
 * @crtc_index: 要等待的 CRTC 索引
 *
 * 阻塞调用者直到指定的 CRTC 经历了一次 VBLANK（垂直消隐期）。
 * 如果 CRTC 被禁用则直接返回（不报错）。
 * 如果有其他 DRM master 存在，返回 -EBUSY。
 *
 * 此函数通过限制更新频率到 VBLANK 间隔来避免与用户空间合成器
 * 产生竞争条件。在等待 VBLANK 期间如果有 DRM master 存在，
 * 可能会造成干扰，因此检测到有外部 master 时就不等待。
 *
 * 返回值：
 * 成功返回 0，失败返回负的错误码。
 */
int drm_client_modeset_wait_for_vblank(struct drm_client_dev *client, unsigned int crtc_index)
{
	struct drm_device *dev = client->dev;
	struct drm_crtc *crtc;
	int ret;

	/*
	 * Rate-limit update frequency to vblank. If there's a DRM master
	 * present, it could interfere while we're waiting for the vblank
	 * event. Don't wait in this case.
	 */
	if (!drm_master_internal_acquire(dev))
		return -EBUSY;

	crtc = client->modesets[crtc_index].crtc;

	/*
	 * Only wait for a vblank event if the CRTC is enabled, otherwise
	 * just don't do anything, not even report an error.
	 */
	ret = drm_crtc_vblank_get(crtc);
	if (!ret) {
		drm_crtc_wait_one_vblank(crtc);
		drm_crtc_vblank_put(crtc);
	}

	drm_master_internal_release(dev);

	return 0;
}
EXPORT_SYMBOL(drm_client_modeset_wait_for_vblank);

#ifdef CONFIG_DRM_KUNIT_TEST
#include "tests/drm_client_modeset_test.c"
#endif
