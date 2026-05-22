/*
 * Copyright (c) 2016 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

/*
 * DRM 属性管理系统 - 中文注释补充
 *
 * 本文件实现了 DRM 核心属性（property）机制。属性是 DRM 展示子系统
 * 用于扩展模式设置接口的核心抽象，用户空间通过属性来传输和查询
 * 模式设置的元数据。
 *
 * 主要功能：
 *   1. 属性的创建和销毁，支持多种属性类型：
 *      - 范围属性（Range）：无符号/有符号整数范围
 *      - 枚举属性（Enum）：预定义的枚举值
 *      - 位掩码属性（Bitmask）：可组合的位标志
 *      - 对象属性（Object）：引用其他模式对象
 *      - 布尔属性（Bool）：0/1 值
 *      - Blob 属性（Blob）：大块二进制数据（如 gamma 表、EDID）
 *   2. Blob 属性生命周期管理（引用计数）
 *   3. 属性值的合法性验证
 *   4. 用户空间 IOCTL 接口（get/set/destroy blob）
 *
 * 属性本身不直接存储当前值，需要通过 drm_object_attach_property()
 * 挂载到具体的模式对象上。属性值统一为 64 位整数，大块数据通过
 * Blob 属性传递。
 *
 * 原子（Atomic）模式设置中，属性是用户空间传递新配置的唯一方式。
 */

#include <linux/export.h>
#include <linux/uaccess.h>

#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_print.h>
#include <drm/drm_property.h>

#include "drm_crtc_internal.h"

/**
 * DOC: overview
 *
 * Properties as represented by &drm_property are used to extend the modeset
 * interface exposed to userspace. For the atomic modeset IOCTL properties are
 * even the only way to transport metadata about the desired new modeset
 * configuration from userspace to the kernel. Properties have a well-defined
 * value range, which is enforced by the drm core. See the documentation of the
 * flags member of &struct drm_property for an overview of the different
 * property types and ranges.
 *
 * Properties don't store the current value directly, but need to be
 * instantiated by attaching them to a &drm_mode_object with
 * drm_object_attach_property().
 *
 * Property values are only 64bit. To support bigger piles of data (like gamma
 * tables, color correction matrices or large structures) a property can instead
 * point at a &drm_property_blob with that additional data.
 *
 * Properties are defined by their symbolic name, userspace must keep a
 * per-object mapping from those names to the property ID used in the atomic
 * IOCTL and in the get/set property IOCTL.
 */

static bool drm_property_flags_valid(u32 flags)
{
	u32 legacy_type = flags & DRM_MODE_PROP_LEGACY_TYPE;
	u32 ext_type = flags & DRM_MODE_PROP_EXTENDED_TYPE;

	/* Reject undefined/deprecated flags */
	if (flags & ~(DRM_MODE_PROP_LEGACY_TYPE |
		      DRM_MODE_PROP_EXTENDED_TYPE |
		      DRM_MODE_PROP_IMMUTABLE |
		      DRM_MODE_PROP_ATOMIC))
		return false;

	/* We want either a legacy type or an extended type, but not both */
	if (!legacy_type == !ext_type)
		return false;

	/* Only one legacy type at a time please */
	if (legacy_type && !is_power_of_2(legacy_type))
		return false;

	return true;
}

/**
 * drm_property_create - create a new property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 * @num_values: number of pre-defined values
 *
 * This creates a new generic drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建通用 DRM 属性的核心函数。
 * flags 指定属性类型（范围/枚举/位掩码/对象/blob 等）和行为（不可变/原子）。
 * num_values 用于预分配 values 数组，对范围属性设为 2（存 min/max），
 * 枚举/位掩码设为枚举项数量。属性创建后自动加入设备的 property_list。
 */
struct drm_property *drm_property_create(struct drm_device *dev,
					 u32 flags, const char *name,
					 int num_values)
{
	struct drm_property *property = NULL;
	int ret;

	if (WARN_ON(!drm_property_flags_valid(flags)))
		return NULL;

	if (WARN_ON(strlen(name) >= DRM_PROP_NAME_LEN))
		return NULL;

	property = kzalloc_obj(struct drm_property);
	if (!property)
		return NULL;

	property->dev = dev;

	if (num_values) {
		property->values = kcalloc(num_values, sizeof(uint64_t),
					   GFP_KERNEL);
		if (!property->values)
			goto fail;
	}

	ret = drm_mode_object_add(dev, &property->base, DRM_MODE_OBJECT_PROPERTY);
	if (ret)
		goto fail;

	property->flags = flags;
	property->num_values = num_values;
	INIT_LIST_HEAD(&property->enum_list);

	strscpy_pad(property->name, name, DRM_PROP_NAME_LEN);

	list_add_tail(&property->head, &dev->mode_config.property_list);

	return property;
fail:
	kfree(property->values);
	kfree(property);
	return NULL;
}
EXPORT_SYMBOL(drm_property_create);

/**
 * drm_property_create_enum - create a new enumeration property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 * @props: enumeration lists with property values
 * @num_values: number of pre-defined values
 *
 * This creates a new generic drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * Userspace is only allowed to set one of the predefined values for enumeration
 * properties.
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建枚举类型属性。用户空间只能设置为预定义值之一。
 * 典型用途：连接器类型、平面混合模式、色彩编码格式等。
 * props 数组由 drm_prop_enum_list 结构组成，包含 (value, name) 对。
 */
struct drm_property *drm_property_create_enum(struct drm_device *dev,
					      u32 flags, const char *name,
					      const struct drm_prop_enum_list *props,
					      int num_values)
{
	struct drm_property *property;
	int i, ret;

	flags |= DRM_MODE_PROP_ENUM;

	property = drm_property_create(dev, flags, name, num_values);
	if (!property)
		return NULL;

	for (i = 0; i < num_values; i++) {
		ret = drm_property_add_enum(property,
					    props[i].type,
					    props[i].name);
		if (ret) {
			drm_property_destroy(dev, property);
			return NULL;
		}
	}

	return property;
}
EXPORT_SYMBOL(drm_property_create_enum);

/**
 * drm_property_create_bitmask - create a new bitmask property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 * @props: enumeration lists with property bitflags
 * @num_props: size of the @props array
 * @supported_bits: bitmask of all supported enumeration values
 *
 * This creates a new bitmask drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * Compared to plain enumeration properties userspace is allowed to set any
 * or'ed together combination of the predefined property bitflag values
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建位掩码类型属性。与枚举不同，用户空间可以设置任意
 * 预定义值的组合（按位或）。典型用途：平面旋转/反射标志组合。
 * supported_bits 声明哪些位是有效的，num_props 中的位若不在其中则被跳过。
 */
struct drm_property *drm_property_create_bitmask(struct drm_device *dev,
						 u32 flags, const char *name,
						 const struct drm_prop_enum_list *props,
						 int num_props,
						 uint64_t supported_bits)
{
	struct drm_property *property;
	int i, ret;
	int num_values = hweight64(supported_bits);

	flags |= DRM_MODE_PROP_BITMASK;

	property = drm_property_create(dev, flags, name, num_values);
	if (!property)
		return NULL;
	for (i = 0; i < num_props; i++) {
		if (!(supported_bits & (1ULL << props[i].type)))
			continue;

		ret = drm_property_add_enum(property,
					    props[i].type,
					    props[i].name);
		if (ret) {
			drm_property_destroy(dev, property);
			return NULL;
		}
	}

	return property;
}
EXPORT_SYMBOL(drm_property_create_bitmask);

static struct drm_property *property_create_range(struct drm_device *dev,
						  u32 flags, const char *name,
						  uint64_t min, uint64_t max)
{
	struct drm_property *property;

	property = drm_property_create(dev, flags, name, 2);
	if (!property)
		return NULL;

	property->values[0] = min;
	property->values[1] = max;

	return property;
}

/**
 * drm_property_create_range - create a new unsigned ranged property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 * @min: minimum value of the property
 * @max: maximum value of the property
 *
 * This creates a new generic drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * Userspace is allowed to set any unsigned integer value in the (min, max)
 * range inclusive.
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建无符号整数范围属性。通过 DRM_MODE_PROP_RANGE 标志
 * 将其标记为范围类型，values[0] 存最小值，values[1] 存最大值。
 * 用户空间可设置 [min, max] 闭区间内的任意整数。
 */
struct drm_property *drm_property_create_range(struct drm_device *dev,
					       u32 flags, const char *name,
					       uint64_t min, uint64_t max)
{
	return property_create_range(dev, DRM_MODE_PROP_RANGE | flags,
			name, min, max);
}
EXPORT_SYMBOL(drm_property_create_range);

/**
 * drm_property_create_signed_range - create a new signed ranged property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 * @min: minimum value of the property
 * @max: maximum value of the property
 *
 * This creates a new generic drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * Userspace is allowed to set any signed integer value in the (min, max)
 * range inclusive.
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建有符号整数范围属性。与无符号版本类似，但使用
 * signed range 标志，且内部通过 I642U64()/U642I64() 进行有符号/无符号转换。
 * 用户空间可设置 [min, max] 闭区间内的任意有符号整数。
 */
struct drm_property *drm_property_create_signed_range(struct drm_device *dev,
						      u32 flags, const char *name,
						      int64_t min, int64_t max)
{
	return property_create_range(dev, DRM_MODE_PROP_SIGNED_RANGE | flags,
			name, I642U64(min), I642U64(max));
}
EXPORT_SYMBOL(drm_property_create_signed_range);

/**
 * drm_property_create_object - create a new object property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 * @type: object type from DRM_MODE_OBJECT_* defines
 *
 * This creates a new generic drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * Userspace is only allowed to set this to any property value of the given
 * @type. Only useful for atomic properties, which is enforced.
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建对象引用类型属性。该属性的值引用另一个 DRM 模式对象
 * （如 CRTC/Connector/Plane）的 ID。仅用于原子模式设置。
 * type 参数指定允许引用的对象类型（DRM_MODE_OBJECT_*）。
 */
struct drm_property *drm_property_create_object(struct drm_device *dev,
						u32 flags, const char *name,
						uint32_t type)
{
	struct drm_property *property;

	flags |= DRM_MODE_PROP_OBJECT;

	if (WARN_ON(!(flags & DRM_MODE_PROP_ATOMIC)))
		return NULL;

	property = drm_property_create(dev, flags, name, 1);
	if (!property)
		return NULL;

	property->values[0] = type;

	return property;
}
EXPORT_SYMBOL(drm_property_create_object);

/**
 * drm_property_create_bool - create a new boolean property type
 * @dev: drm device
 * @flags: flags specifying the property type
 * @name: name of the property
 *
 * This creates a new generic drm property which can then be attached to a drm
 * object with drm_object_attach_property(). The returned property object must
 * be freed with drm_property_destroy(), which is done automatically when
 * calling drm_mode_config_cleanup().
 *
 * This is implemented as a ranged property with only {0, 1} as valid values.
 *
 * Returns:
 * A pointer to the newly created property on success, NULL on failure.
 */
/*
 * 中文说明：创建布尔类型属性。实现上是对 drm_property_create_range()
 * 的封装，将有效值限定为 {0, 1}，分别表示 false 和 true。
 */
struct drm_property *drm_property_create_bool(struct drm_device *dev,
					      u32 flags, const char *name)
{
	return drm_property_create_range(dev, flags, name, 0, 1);
}
EXPORT_SYMBOL(drm_property_create_bool);

/**
 * drm_property_add_enum - add a possible value to an enumeration property
 * @property: enumeration property to change
 * @value: value of the new enumeration
 * @name: symbolic name of the new enumeration
 *
 * This functions adds enumerations to a property.
 *
 * It's use is deprecated, drivers should use one of the more specific helpers
 * to directly create the property with all enumerations already attached.
 *
 * Returns:
 * Zero on success, error code on failure.
 */
/*
 * 中文说明：向枚举或位掩码属性添加一个枚举值。
 * value 是数值，name 是用户空间看到的字符串名。
 * 同一属性中不允许重复的 value，位掩码属性的 value 必须在 0~63 范围内。
 * 此函数已废弃，建议使用一次性创建函数（如 drm_property_create_enum）。
 */
int drm_property_add_enum(struct drm_property *property,
			  uint64_t value, const char *name)
{
	struct drm_property_enum *prop_enum;
	int index = 0;

	if (WARN_ON(strlen(name) >= DRM_PROP_NAME_LEN))
		return -EINVAL;

	if (WARN_ON(!drm_property_type_is(property, DRM_MODE_PROP_ENUM) &&
		    !drm_property_type_is(property, DRM_MODE_PROP_BITMASK)))
		return -EINVAL;

	/*
	 * Bitmask enum properties have the additional constraint of values
	 * from 0 to 63
	 */
	if (WARN_ON(drm_property_type_is(property, DRM_MODE_PROP_BITMASK) &&
		    value > 63))
		return -EINVAL;

	list_for_each_entry(prop_enum, &property->enum_list, head) {
		if (WARN_ON(prop_enum->value == value))
			return -EINVAL;
		index++;
	}

	if (WARN_ON(index >= property->num_values))
		return -EINVAL;

	prop_enum = kzalloc_obj(struct drm_property_enum);
	if (!prop_enum)
		return -ENOMEM;

	strscpy_pad(prop_enum->name, name, DRM_PROP_NAME_LEN);
	prop_enum->value = value;

	property->values[index] = value;
	list_add_tail(&prop_enum->head, &property->enum_list);
	return 0;
}
EXPORT_SYMBOL(drm_property_add_enum);

/**
 * drm_property_destroy - destroy a drm property
 * @dev: drm device
 * @property: property to destroy
 *
 * This function frees a property including any attached resources like
 * enumeration values.
 */
/*
 * 中文说明：销毁 DRM 属性。释放枚举列表、values 数组，
 * 从模式对象 IDR 中注销，并从设备的 property_list 中移除。
 * 通常在 drm_mode_config_cleanup() 中自动调用。
 */
void drm_property_destroy(struct drm_device *dev, struct drm_property *property)
{
	struct drm_property_enum *prop_enum, *pt;

	list_for_each_entry_safe(prop_enum, pt, &property->enum_list, head) {
		list_del(&prop_enum->head);
		kfree(prop_enum);
	}

	if (property->num_values)
		kfree(property->values);
	drm_mode_object_unregister(dev, &property->base);
	list_del(&property->head);
	kfree(property);
}
EXPORT_SYMBOL(drm_property_destroy);

int drm_mode_getproperty_ioctl(struct drm_device *dev,
			       void *data, struct drm_file *file_priv)
{
	struct drm_mode_get_property *out_resp = data;
	struct drm_property *property;
	int enum_count = 0;
	int value_count = 0;
	int i, copied;
	struct drm_property_enum *prop_enum;
	struct drm_mode_property_enum __user *enum_ptr;
	uint64_t __user *values_ptr;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -EOPNOTSUPP;

	property = drm_property_find(dev, file_priv, out_resp->prop_id);
	if (!property)
		return -ENOENT;

	strscpy_pad(out_resp->name, property->name, DRM_PROP_NAME_LEN);
	out_resp->flags = property->flags;

	value_count = property->num_values;
	values_ptr = u64_to_user_ptr(out_resp->values_ptr);

	for (i = 0; i < value_count; i++) {
		if (i < out_resp->count_values &&
		    put_user(property->values[i], values_ptr + i)) {
			return -EFAULT;
		}
	}
	out_resp->count_values = value_count;

	copied = 0;
	enum_ptr = u64_to_user_ptr(out_resp->enum_blob_ptr);

	if (drm_property_type_is(property, DRM_MODE_PROP_ENUM) ||
	    drm_property_type_is(property, DRM_MODE_PROP_BITMASK)) {
		list_for_each_entry(prop_enum, &property->enum_list, head) {
			enum_count++;
			if (out_resp->count_enum_blobs < enum_count)
				continue;

			if (copy_to_user(&enum_ptr[copied].value,
					 &prop_enum->value, sizeof(uint64_t)))
				return -EFAULT;

			if (copy_to_user(&enum_ptr[copied].name,
					 &prop_enum->name, DRM_PROP_NAME_LEN))
				return -EFAULT;
			copied++;
		}
		out_resp->count_enum_blobs = enum_count;
	}

	/*
	 * NOTE: The idea seems to have been to use this to read all the blob
	 * property values. But nothing ever added them to the corresponding
	 * list, userspace always used the special-purpose get_blob ioctl to
	 * read the value for a blob property. It also doesn't make a lot of
	 * sense to return values here when everything else is just metadata for
	 * the property itself.
	 */
	if (drm_property_type_is(property, DRM_MODE_PROP_BLOB))
		out_resp->count_enum_blobs = 0;

	return 0;
}

static void drm_property_free_blob(struct kref *kref)
{
	struct drm_property_blob *blob =
		container_of(kref, struct drm_property_blob, base.refcount);

	mutex_lock(&blob->dev->mode_config.blob_lock);
	list_del(&blob->head_global);
	mutex_unlock(&blob->dev->mode_config.blob_lock);

	drm_mode_object_unregister(blob->dev, &blob->base);

	kvfree(blob);
}

/**
 * drm_property_create_blob - Create new blob property
 * @dev: DRM device to create property for
 * @length: Length to allocate for blob data
 * @data: If specified, copies data into blob
 *
 * Creates a new blob property for a specified DRM device, optionally
 * copying data. Note that blob properties are meant to be invariant, hence the
 * data must be filled out before the blob is used as the value of any property.
 *
 * Returns:
 * New blob property with a single reference on success, or an ERR_PTR
 * value on failure.
 */
/*
 * 中文说明：创建 Blob 类型属性。Blob 用于传输大块数据
 * （如 gamma LUT、CTM 矩阵、EDID 等）。blob 数据在创建后不可变，
 * 替换时需要创建新的 blob 再替换旧 blob 的引用。
 * blob 通过引用计数管理生命周期，dev->mode_config.blob_lock 保护链表。
 */
struct drm_property_blob *
drm_property_create_blob(struct drm_device *dev, size_t length,
			 const void *data)
{
	struct drm_property_blob *blob;
	int ret;

	if (!length || length > INT_MAX - sizeof(struct drm_property_blob))
		return ERR_PTR(-EINVAL);

	blob = kvzalloc(sizeof(struct drm_property_blob) + length, GFP_KERNEL_ACCOUNT);
	if (!blob)
		return ERR_PTR(-ENOMEM);

	/* This must be explicitly initialised, so we can safely call list_del
	 * on it in the removal handler, even if it isn't in a file list. */
	INIT_LIST_HEAD(&blob->head_file);
	blob->data = (void *)blob + sizeof(*blob);
	blob->length = length;
	blob->dev = dev;

	if (data)
		memcpy(blob->data, data, length);

	ret = __drm_mode_object_add(dev, &blob->base, DRM_MODE_OBJECT_BLOB,
				    true, drm_property_free_blob);
	if (ret) {
		kvfree(blob);
		return ERR_PTR(-EINVAL);
	}

	mutex_lock(&dev->mode_config.blob_lock);
	list_add_tail(&blob->head_global,
	              &dev->mode_config.property_blob_list);
	mutex_unlock(&dev->mode_config.blob_lock);

	return blob;
}
EXPORT_SYMBOL(drm_property_create_blob);

/**
 * drm_property_blob_put - release a blob property reference
 * @blob: DRM blob property
 *
 * Releases a reference to a blob property. May free the object.
 */
/*
 * 中文说明：释放 blob 属性的一个引用。当引用计数归零时，
 * 自动从全局 blob 列表移除并释放内存。允许传入 NULL。
 */
void drm_property_blob_put(struct drm_property_blob *blob)
{
	if (!blob)
		return;

	drm_mode_object_put(&blob->base);
}
EXPORT_SYMBOL(drm_property_blob_put);

void drm_property_destroy_user_blobs(struct drm_device *dev,
				     struct drm_file *file_priv)
{
	struct drm_property_blob *blob, *bt;

	/*
	 * When the file gets released that means no one else can access the
	 * blob list any more, so no need to grab dev->blob_lock.
	 */
	list_for_each_entry_safe(blob, bt, &file_priv->blobs, head_file) {
		list_del_init(&blob->head_file);
		drm_property_blob_put(blob);
	}
}

/**
 * drm_property_blob_get - acquire blob property reference
 * @blob: DRM blob property
 *
 * Acquires a reference to an existing blob property. Returns @blob, which
 * allows this to be used as a shorthand in assignments.
 */
/*
 * 中文说明：增加 blob 属性的引用计数。返回 blob 本身，
 * 方便链式赋值。与 drm_property_blob_put() 配对使用。
 */
struct drm_property_blob *drm_property_blob_get(struct drm_property_blob *blob)
{
	drm_mode_object_get(&blob->base);
	return blob;
}
EXPORT_SYMBOL(drm_property_blob_get);

/**
 * drm_property_lookup_blob - look up a blob property and take a reference
 * @dev: drm device
 * @id: id of the blob property
 *
 * If successful, this takes an additional reference to the blob property.
 * callers need to make sure to eventually unreferenced the returned property
 * again, using drm_property_blob_put().
 *
 * Return:
 * NULL on failure, pointer to the blob on success.
 */
/*
 * 中文说明：通过 ID 查找 blob 属性并增加引用计数。
 * 调用者必须在用完后通过 drm_property_blob_put() 释放引用。
 * 内部使用 __drm_mode_object_find() 在 IDR 中查找。
 */
struct drm_property_blob *drm_property_lookup_blob(struct drm_device *dev,
					           uint32_t id)
{
	struct drm_mode_object *obj;
	struct drm_property_blob *blob = NULL;

	obj = __drm_mode_object_find(dev, NULL, id, DRM_MODE_OBJECT_BLOB);
	if (obj)
		blob = obj_to_blob(obj);
	return blob;
}
EXPORT_SYMBOL(drm_property_lookup_blob);

/**
 * drm_property_replace_global_blob - replace existing blob property
 * @dev: drm device
 * @replace: location of blob property pointer to be replaced
 * @length: length of data for new blob, or 0 for no data
 * @data: content for new blob, or NULL for no data
 * @obj_holds_id: optional object for property holding blob ID
 * @prop_holds_id: optional property holding blob ID
 * @return 0 on success or error on failure
 *
 * This function will replace a global property in the blob list, optionally
 * updating a property which holds the ID of that property.
 *
 * If length is 0 or data is NULL, no new blob will be created, and the holding
 * property, if specified, will be set to 0.
 *
 * Access to the replace pointer is assumed to be protected by the caller, e.g.
 * by holding the relevant modesetting object lock for its parent.
 *
 * For example, a drm_connector has a 'PATH' property, which contains the ID
 * of a blob property with the value of the MST path information. Calling this
 * function with replace pointing to the connector's path_blob_ptr, length and
 * data set for the new path information, obj_holds_id set to the connector's
 * base object, and prop_holds_id set to the path property name, will perform
 * a completely atomic update. The access to path_blob_ptr is protected by the
 * caller holding a lock on the connector.
 */
/*
 * 中文说明：全局 blob 属性替换函数。用于原子性地替换 blob 并更新
 * 持有其 ID 的属性。典型场景：更新连接器的 PATH 属性（MST 路径）。
 * 如果 length/data 为空，则将持有属性置为 0（即清除 blob 引用）。
 * 调用者必须确保对 replace 指针的访问已加锁保护。
 */
int drm_property_replace_global_blob(struct drm_device *dev,
				     struct drm_property_blob **replace,
				     size_t length,
				     const void *data,
				     struct drm_mode_object *obj_holds_id,
				     struct drm_property *prop_holds_id)
{
	struct drm_property_blob *new_blob = NULL;
	struct drm_property_blob *old_blob = NULL;
	int ret;

	WARN_ON(replace == NULL);

	old_blob = *replace;

	if (length && data) {
		new_blob = drm_property_create_blob(dev, length, data);
		if (IS_ERR(new_blob))
			return PTR_ERR(new_blob);
	}

	if (obj_holds_id) {
		ret = drm_object_property_set_value(obj_holds_id,
						    prop_holds_id,
						    new_blob ?
						        new_blob->base.id : 0);
		if (ret != 0)
			goto err_created;
	}

	drm_property_blob_put(old_blob);
	*replace = new_blob;

	return 0;

err_created:
	drm_property_blob_put(new_blob);
	return ret;
}
EXPORT_SYMBOL(drm_property_replace_global_blob);

/**
 * drm_property_replace_blob - replace a blob property
 * @blob: a pointer to the member blob to be replaced
 * @new_blob: the new blob to replace with
 *
 * Return: true if the blob was in fact replaced.
 */
/*
 * 中文说明：替换 blob 属性指针。释放旧 blob 的引用，
 * 获取新 blob 的引用（如果非 NULL）。如果新旧相同则跳过。
 * 返回是否实际发生了替换。
 */
bool drm_property_replace_blob(struct drm_property_blob **blob,
			       struct drm_property_blob *new_blob)
{
	struct drm_property_blob *old_blob = *blob;

	if (old_blob == new_blob)
		return false;

	drm_property_blob_put(old_blob);
	if (new_blob)
		drm_property_blob_get(new_blob);
	*blob = new_blob;
	return true;
}
EXPORT_SYMBOL(drm_property_replace_blob);

/**
 * drm_property_replace_blob_from_id - replace a blob property taking a reference
 * @dev: DRM device
 * @blob: a pointer to the member blob to be replaced
 * @blob_id: the id of the new blob to replace with
 * @max_size: the maximum size of the blob property for variable-size blobs
 * @expected_size: expected size of the blob property
 * @expected_elem_size: expected size of an element in the blob property
 * @replaced: if the blob was in fact replaced
 *
 * Look up the new blob from id, take its reference, check expected sizes of
 * the blob and its element and replace the old blob by the new one. Advertise
 * if the replacement operation was successful.
 *
 * Return: true if the blob was in fact replaced. -EINVAL if the new blob was
 * not found or sizes don't match.
 */
/*
 * 中文说明：通过 ID 查找并替换 blob 属性。在替换前进行尺寸校验：
 * max_size 限制最大长度，expected_size 要求精确匹配，expected_elem_size 要求长度对齐。
 * 任意一项检查不通过则返回 -EINVAL。blob_id 为 0 表示清除 blob。
 */
int drm_property_replace_blob_from_id(struct drm_device *dev,
					 struct drm_property_blob **blob,
					 uint64_t blob_id,
					 ssize_t max_size,
					 ssize_t expected_size,
					 ssize_t expected_elem_size,
					 bool *replaced)
{
	struct drm_property_blob *new_blob = NULL;

	if (blob_id != 0) {
		new_blob = drm_property_lookup_blob(dev, blob_id);
		if (new_blob == NULL) {
			drm_dbg_atomic(dev,
				       "cannot find blob ID %llu\n", blob_id);
			return -EINVAL;
		}

		if (max_size > 0 &&
		    new_blob->length > max_size) {
			drm_dbg_atomic(dev,
				       "[BLOB:%d] length %zu greater than max %zu\n",
				       new_blob->base.id, new_blob->length, max_size);
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}

		if (expected_size > 0 &&
		    new_blob->length != expected_size) {
			drm_dbg_atomic(dev,
				       "[BLOB:%d] length %zu different from expected %zu\n",
				       new_blob->base.id, new_blob->length, expected_size);
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
		if (expected_elem_size > 0 &&
		    new_blob->length % expected_elem_size != 0) {
			drm_dbg_atomic(dev,
				       "[BLOB:%d] length %zu not divisible by element size %zu\n",
				       new_blob->base.id, new_blob->length, expected_elem_size);
			drm_property_blob_put(new_blob);
			return -EINVAL;
		}
	}

	*replaced |= drm_property_replace_blob(blob, new_blob);
	drm_property_blob_put(new_blob);

	return 0;
}
EXPORT_SYMBOL(drm_property_replace_blob_from_id);

int drm_mode_getblob_ioctl(struct drm_device *dev,
			   void *data, struct drm_file *file_priv)
{
	struct drm_mode_get_blob *out_resp = data;
	struct drm_property_blob *blob;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -EOPNOTSUPP;

	blob = drm_property_lookup_blob(dev, out_resp->blob_id);
	if (!blob)
		return -ENOENT;

	if (out_resp->length == blob->length) {
		if (copy_to_user(u64_to_user_ptr(out_resp->data),
				 blob->data,
				 blob->length)) {
			ret = -EFAULT;
			goto unref;
		}
	}
	out_resp->length = blob->length;
unref:
	drm_property_blob_put(blob);

	return ret;
}

int drm_mode_createblob_ioctl(struct drm_device *dev,
			      void *data, struct drm_file *file_priv)
{
	struct drm_mode_create_blob *out_resp = data;
	struct drm_property_blob *blob;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -EOPNOTSUPP;

	blob = drm_property_create_blob(dev, out_resp->length, NULL);
	if (IS_ERR(blob))
		return PTR_ERR(blob);

	if (copy_from_user(blob->data,
			   u64_to_user_ptr(out_resp->data),
			   out_resp->length)) {
		ret = -EFAULT;
		goto out_blob;
	}

	/* Dropping the lock between create_blob and our access here is safe
	 * as only the same file_priv can remove the blob; at this point, it is
	 * not associated with any file_priv. */
	mutex_lock(&dev->mode_config.blob_lock);
	out_resp->blob_id = blob->base.id;
	list_add_tail(&blob->head_file, &file_priv->blobs);
	mutex_unlock(&dev->mode_config.blob_lock);

	return 0;

out_blob:
	drm_property_blob_put(blob);
	return ret;
}

int drm_mode_destroyblob_ioctl(struct drm_device *dev,
			       void *data, struct drm_file *file_priv)
{
	struct drm_mode_destroy_blob *out_resp = data;
	struct drm_property_blob *blob = NULL, *bt;
	bool found = false;
	int ret = 0;

	if (!drm_core_check_feature(dev, DRIVER_MODESET))
		return -EOPNOTSUPP;

	blob = drm_property_lookup_blob(dev, out_resp->blob_id);
	if (!blob)
		return -ENOENT;

	mutex_lock(&dev->mode_config.blob_lock);
	/* Ensure the property was actually created by this user. */
	list_for_each_entry(bt, &file_priv->blobs, head_file) {
		if (bt == blob) {
			found = true;
			break;
		}
	}

	if (!found) {
		ret = -EPERM;
		goto err;
	}

	/* We must drop head_file here, because we may not be the last
	 * reference on the blob. */
	list_del_init(&blob->head_file);
	mutex_unlock(&dev->mode_config.blob_lock);

	/* One reference from lookup, and one from the filp. */
	drm_property_blob_put(blob);
	drm_property_blob_put(blob);

	return 0;

err:
	mutex_unlock(&dev->mode_config.blob_lock);
	drm_property_blob_put(blob);

	return ret;
}

/* Some properties could refer to dynamic refcnt'd objects, or things that
 * need special locking to handle lifetime issues (ie. to ensure the prop
 * value doesn't become invalid part way through the property update due to
 * race).  The value returned by reference via 'obj' should be passed back
 * to drm_property_change_valid_put() after the property is set (and the
 * object to which the property is attached has a chance to take its own
 * reference).
 */
bool drm_property_change_valid_get(struct drm_property *property,
				   uint64_t value, struct drm_mode_object **ref)
{
	int i;

	if (property->flags & DRM_MODE_PROP_IMMUTABLE)
		return false;

	*ref = NULL;

	if (drm_property_type_is(property, DRM_MODE_PROP_RANGE)) {
		if (value < property->values[0] || value > property->values[1])
			return false;
		return true;
	} else if (drm_property_type_is(property, DRM_MODE_PROP_SIGNED_RANGE)) {
		int64_t svalue = U642I64(value);

		if (svalue < U642I64(property->values[0]) ||
				svalue > U642I64(property->values[1]))
			return false;
		return true;
	} else if (drm_property_type_is(property, DRM_MODE_PROP_BITMASK)) {
		uint64_t valid_mask = 0;

		for (i = 0; i < property->num_values; i++)
			valid_mask |= (1ULL << property->values[i]);
		return !(value & ~valid_mask);
	} else if (drm_property_type_is(property, DRM_MODE_PROP_BLOB)) {
		struct drm_property_blob *blob;

		if (value == 0)
			return true;

		blob = drm_property_lookup_blob(property->dev, value);
		if (blob) {
			*ref = &blob->base;
			return true;
		} else {
			return false;
		}
	} else if (drm_property_type_is(property, DRM_MODE_PROP_OBJECT)) {
		/* a zero value for an object property translates to null: */
		if (value == 0)
			return true;

		*ref = __drm_mode_object_find(property->dev, NULL, value,
					      property->values[0]);
		return *ref != NULL;
	}

	for (i = 0; i < property->num_values; i++)
		if (property->values[i] == value)
			return true;
	return false;
}

void drm_property_change_valid_put(struct drm_property *property,
		struct drm_mode_object *ref)
{
	if (!ref)
		return;

	if (drm_property_type_is(property, DRM_MODE_PROP_OBJECT)) {
		drm_mode_object_put(ref);
	} else if (drm_property_type_is(property, DRM_MODE_PROP_BLOB))
		drm_property_blob_put(obj_to_blob(ref));
}
