// SPDX-License-Identifier: GPL-2.0+
/*
 * vsp1_entity.c  --  R-Car VSP1 Base Entity
 *
 * Copyright (C) 2013-2014 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/device.h>
#include <linux/gfp.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

#include "vsp1.h"
#include "vsp1_dl.h"
#include "vsp1_entity.h"
#include "vsp1_pipe.h"
#include "vsp1_rwpf.h"

void vsp1_entity_route_setup(struct vsp1_entity *entity,
			     struct vsp1_pipeline *pipe,
			     struct vsp1_dl_body *dlb)
{
	struct vsp1_entity *source;
	u32 route;

	if (entity->type == VSP1_ENTITY_HGO) {
		u32 smppt;

		/*
		 * The HGO is a special case, its routing is configured on the
		 * sink pad.
		 */
		source = entity->sources[0];
		smppt = (pipe->output->entity.index << VI6_DPR_SMPPT_TGW_SHIFT)
		      | (source->route->output << VI6_DPR_SMPPT_PT_SHIFT);

		vsp1_dl_body_write(dlb, VI6_DPR_HGO_SMPPT, smppt);
		return;
	} else if (entity->type == VSP1_ENTITY_HGT) {
		u32 smppt;

		/*
		 * The HGT is a special case, its routing is configured on the
		 * sink pad.
		 */
		source = entity->sources[0];
		smppt = (pipe->output->entity.index << VI6_DPR_SMPPT_TGW_SHIFT)
		      | (source->route->output << VI6_DPR_SMPPT_PT_SHIFT);

		vsp1_dl_body_write(dlb, VI6_DPR_HGT_SMPPT, smppt);
		return;
	}

	source = entity;
	if (source->route->reg == 0)
		return;

	route = source->sink->route->inputs[source->sink_pad];
	/*
	 * The ILV and BRS share the same data path route. The extra BRSSEL bit
	 * selects between the ILV and BRS.
	 *
	 * The BRU and IIF share the same data path route. The extra IIFSEL bit
	 * selects between the IIF and BRU.
	 */
	if (source->type == VSP1_ENTITY_BRS)
		route |= VI6_DPR_ROUTE_BRSSEL;
	else if (source->type == VSP1_ENTITY_IIF)
		route |= VI6_DPR_ROUTE_IIFSEL;
	vsp1_dl_body_write(dlb, source->route->reg, route);
}

void vsp1_entity_configure_stream(struct vsp1_entity *entity,
				  struct v4l2_subdev_state *state,
				  struct vsp1_pipeline *pipe,
				  struct vsp1_dl_list *dl,
				  struct vsp1_dl_body *dlb)
{
	if (entity->ops->configure_stream)
		entity->ops->configure_stream(entity, state, pipe, dl, dlb);
}

void vsp1_entity_configure_frame(struct vsp1_entity *entity,
				 struct vsp1_pipeline *pipe,
				 struct vsp1_dl_list *dl,
				 struct vsp1_dl_body *dlb)
{
	if (entity->ops->configure_frame)
		entity->ops->configure_frame(entity, pipe, dl, dlb);
}

void vsp1_entity_configure_partition(struct vsp1_entity *entity,
				     struct vsp1_pipeline *pipe,
				     const struct vsp1_partition *partition,
				     struct vsp1_dl_list *dl,
				     struct vsp1_dl_body *dlb)
{
	if (entity->ops->configure_partition)
		entity->ops->configure_partition(entity, pipe, partition,
						 dl, dlb);
}

void vsp1_entity_adjust_color_space(struct v4l2_mbus_framefmt *format)
{
	u8 xfer_func = format->xfer_func;
	u8 ycbcr_enc = format->ycbcr_enc;
	u8 quantization = format->quantization;

	vsp1_adjust_color_space(format->code, &format->colorspace, &xfer_func,
				&ycbcr_enc, &quantization);

	format->xfer_func = xfer_func;
	format->ycbcr_enc = ycbcr_enc;
	format->quantization = quantization;
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdevice Operations
 */

/**
 * vsp1_entity_get_state - Get the subdev state for an entity
 * @entity: the entity
 * @sd_state: the TRY state
 * @which: state selector (ACTIVE or TRY)
 *
 * When called with which set to V4L2_SUBDEV_FORMAT_ACTIVE the caller must hold
 * the entity lock to access the returned configuration.
 *
 * Return the subdev state requested by the which argument. The TRY state is
 * passed explicitly to the function through the sd_state argument and simply
 * returned when requested. The ACTIVE state comes from the entity structure.
 */
struct v4l2_subdev_state *
vsp1_entity_get_state(struct vsp1_entity *entity,
		      struct v4l2_subdev_state *sd_state,
		      enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return entity->state;
	case V4L2_SUBDEV_FORMAT_TRY:
	default:
		return sd_state;
	}
}

/*
 * vsp1_subdev_get_pad_format - Subdev pad get_fmt handler
 * @subdev: V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: V4L2 subdev format
 *
 * This function implements the subdev get_fmt pad operation. It can be used as
 * a direct drop-in for the operation handler.
 */
int vsp1_subdev_get_pad_format(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_format *fmt)
{
	struct vsp1_entity *entity = to_vsp1_entity(subdev);
	struct v4l2_subdev_state *state;

	state = vsp1_entity_get_state(entity, sd_state, fmt->which);
	if (!state)
		return -EINVAL;

	mutex_lock(&entity->lock);
	fmt->format = *v4l2_subdev_state_get_format(state, fmt->pad);
	mutex_unlock(&entity->lock);

	return 0;
}

/*
 * vsp1_subdev_enum_mbus_code - Subdev pad enum_mbus_code handler
 * @subdev: V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @code: Media bus code enumeration
 * @codes: Array of supported media bus codes
 * @ncodes: Number of supported media bus codes
 *
 * This function implements the subdev enum_mbus_code pad operation for entities
 * that do not support format conversion. It enumerates the given supported
 * media bus codes on the sink pad and reports a source pad format identical to
 * the sink pad.
 */
int vsp1_subdev_enum_mbus_code(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_mbus_code_enum *code,
			       const unsigned int *codes, unsigned int ncodes)
{
	struct vsp1_entity *entity = to_vsp1_entity(subdev);

	if (code->pad == 0) {
		if (code->index >= ncodes)
			return -EINVAL;

		code->code = codes[code->index];
	} else {
		struct v4l2_subdev_state *state;
		struct v4l2_mbus_framefmt *format;

		/*
		 * The entity can't perform format conversion, the sink format
		 * is always identical to the source format.
		 */
		if (code->index)
			return -EINVAL;

		state = vsp1_entity_get_state(entity, sd_state, code->which);
		if (!state)
			return -EINVAL;

		mutex_lock(&entity->lock);
		format = v4l2_subdev_state_get_format(state, 0);
		code->code = format->code;
		mutex_unlock(&entity->lock);
	}

	return 0;
}

/*
 * vsp1_subdev_enum_frame_size - Subdev pad enum_frame_size handler
 * @subdev: V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fse: Frame size enumeration
 * @min_width: Minimum image width
 * @min_height: Minimum image height
 * @max_width: Maximum image width
 * @max_height: Maximum image height
 *
 * This function implements the subdev enum_frame_size pad operation for
 * entities that do not support scaling or cropping. It reports the given
 * minimum and maximum frame width and height on the sink pad, and a fixed
 * source pad size identical to the sink pad.
 */
int vsp1_subdev_enum_frame_size(struct v4l2_subdev *subdev,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_frame_size_enum *fse,
				unsigned int min_width, unsigned int min_height,
				unsigned int max_width, unsigned int max_height)
{
	struct vsp1_entity *entity = to_vsp1_entity(subdev);
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *format;
	int ret = 0;

	state = vsp1_entity_get_state(entity, sd_state, fse->which);
	if (!state)
		return -EINVAL;

	format = v4l2_subdev_state_get_format(state, fse->pad);

	mutex_lock(&entity->lock);

	if (fse->index || fse->code != format->code) {
		ret = -EINVAL;
		goto done;
	}

	if (fse->pad == 0) {
		fse->min_width = min_width;
		fse->max_width = max_width;
		fse->min_height = min_height;
		fse->max_height = max_height;
	} else {
		/*
		 * The size on the source pad are fixed and always identical to
		 * the size on the sink pad.
		 */
		fse->min_width = format->width;
		fse->max_width = format->width;
		fse->min_height = format->height;
		fse->max_height = format->height;
	}

done:
	mutex_unlock(&entity->lock);
	return ret;
}

/*
 * vsp1_subdev_set_pad_format - Subdev pad set_fmt handler
 * @subdev: V4L2 subdevice
 * @sd_state: V4L2 subdev state
 * @fmt: V4L2 subdev format
 * @codes: Array of supported media bus codes
 * @ncodes: Number of supported media bus codes
 * @min_width: Minimum image width
 * @min_height: Minimum image height
 * @max_width: Maximum image width
 * @max_height: Maximum image height
 *
 * This function implements the subdev set_fmt pad operation for entities that
 * do not support scaling or cropping. It defaults to the first supplied media
 * bus code if the requested code isn't supported, clamps the size to the
 * supplied minimum and maximum, and propagates the sink pad format to the
 * source pad.
 */
int vsp1_subdev_set_pad_format(struct v4l2_subdev *subdev,
			       struct v4l2_subdev_state *sd_state,
			       struct v4l2_subdev_format *fmt,
			       const unsigned int *codes, unsigned int ncodes,
			       unsigned int min_width, unsigned int min_height,
			       unsigned int max_width, unsigned int max_height)
{
	struct vsp1_entity *entity = to_vsp1_entity(subdev);
	struct v4l2_subdev_state *state;
	struct v4l2_mbus_framefmt *format;
	struct v4l2_rect *selection;
	unsigned int i;
	int ret = 0;

	mutex_lock(&entity->lock);

	state = vsp1_entity_get_state(entity, sd_state, fmt->which);
	if (!state) {
		ret = -EINVAL;
		goto done;
	}

	format = v4l2_subdev_state_get_format(state, fmt->pad);

	if (fmt->pad == entity->source_pad) {
		/* The output format can't be modified. */
		fmt->format = *format;
		goto done;
	}

	/*
	 * Default to the first media bus code if the requested format is not
	 * supported.
	 */
	for (i = 0; i < ncodes; ++i) {
		if (fmt->format.code == codes[i])
			break;
	}

	format->code = i < ncodes ? codes[i] : codes[0];
	format->width = clamp_t(unsigned int, fmt->format.width,
				min_width, max_width);
	format->height = clamp_t(unsigned int, fmt->format.height,
				 min_height, max_height);
	format->field = V4L2_FIELD_NONE;

	format->colorspace = fmt->format.colorspace;
	format->xfer_func = fmt->format.xfer_func;
	format->ycbcr_enc = fmt->format.ycbcr_enc;
	format->quantization = fmt->format.quantization;

	vsp1_entity_adjust_color_space(format);

	fmt->format = *format;

	/* Propagate the format to the source pad. */
	format = v4l2_subdev_state_get_format(state, entity->source_pad);
	*format = fmt->format;

	/* Reset the crop and compose rectangles. */
	selection = v4l2_subdev_state_get_crop(state, fmt->pad);
	selection->left = 0;
	selection->top = 0;
	selection->width = format->width;
	selection->height = format->height;

	selection = v4l2_subdev_state_get_compose(state, fmt->pad);
	selection->left = 0;
	selection->top = 0;
	selection->width = format->width;
	selection->height = format->height;

done:
	mutex_unlock(&entity->lock);
	return ret;
}

static int vsp1_entity_init_state(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_state *sd_state)
{
	unsigned int pad;

	/* Initialize all pad formats with default values. */
	for (pad = 0; pad < subdev->entity.num_pads - 1; ++pad) {
		struct v4l2_subdev_format format = {
			.pad = pad,
			.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY
			       : V4L2_SUBDEV_FORMAT_ACTIVE,
		};

		v4l2_subdev_call(subdev, pad, set_fmt, sd_state, &format);
	}

	return 0;
}

static const struct v4l2_subdev_internal_ops vsp1_entity_internal_ops = {
	.init_state = vsp1_entity_init_state,
};

/* -----------------------------------------------------------------------------
 * Media Operations
 */

static inline struct vsp1_entity *
media_entity_to_vsp1_entity(struct media_entity *entity)
{
	return container_of(entity, struct vsp1_entity, subdev.entity);
}

static int vsp1_entity_link_setup_source(const struct media_pad *source_pad,
					 const struct media_pad *sink_pad,
					 u32 flags)
{
	struct vsp1_entity *source;

	source = media_entity_to_vsp1_entity(source_pad->entity);

	if (!source->route)
		return 0;

	if (flags & MEDIA_LNK_FL_ENABLED) {
		struct vsp1_entity *sink
			= media_entity_to_vsp1_entity(sink_pad->entity);

		/*
		 * Fan-out is limited to one for the normal data path plus
		 * optional HGO and HGT. We ignore the HGO and HGT here.
		 */
		if (sink->type != VSP1_ENTITY_HGO &&
		    sink->type != VSP1_ENTITY_HGT) {
			if (source->sink)
				return -EBUSY;
			source->sink = sink;
			source->sink_pad = sink_pad->index;
		}
	} else {
		source->sink = NULL;
		source->sink_pad = 0;
	}

	return 0;
}

static int vsp1_entity_link_setup_sink(const struct media_pad *source_pad,
				       const struct media_pad *sink_pad,
				       u32 flags)
{
	struct vsp1_entity *sink;
	struct vsp1_entity *source;

	sink = media_entity_to_vsp1_entity(sink_pad->entity);
	source = media_entity_to_vsp1_entity(source_pad->entity);

	if (flags & MEDIA_LNK_FL_ENABLED) {
		/* Fan-in is limited to one. */
		if (sink->sources[sink_pad->index])
			return -EBUSY;

		sink->sources[sink_pad->index] = source;
	} else {
		sink->sources[sink_pad->index] = NULL;
	}

	return 0;
}

int vsp1_entity_link_setup(struct media_entity *entity,
			   const struct media_pad *local,
			   const struct media_pad *remote, u32 flags)
{
	if (local->flags & MEDIA_PAD_FL_SOURCE)
		return vsp1_entity_link_setup_source(local, remote, flags);
	else
		return vsp1_entity_link_setup_sink(remote, local, flags);
}

/**
 * vsp1_entity_remote_pad - Find the pad at the remote end of a link
 * @pad: Pad at the local end of the link
 *
 * Search for a remote pad connected to the given pad by iterating over all
 * links originating or terminating at that pad until an enabled link is found.
 *
 * Our link setup implementation guarantees that the output fan-out will not be
 * higher than one for the data pipelines, except for the links to the HGO and
 * HGT that can be enabled in addition to a regular data link. When traversing
 * outgoing links this function ignores HGO and HGT entities and should thus be
 * used in place of the generic media_pad_remote_pad_first() function to
 * traverse data pipelines.
 *
 * Return a pointer to the pad at the remote end of the first found enabled
 * link, or NULL if no enabled link has been found.
 */
struct media_pad *vsp1_entity_remote_pad(struct media_pad *pad)
{
	struct media_link *link;

	list_for_each_entry(link, &pad->entity->links, list) {
		struct vsp1_entity *entity;

		if (!(link->flags & MEDIA_LNK_FL_ENABLED))
			continue;

		/* If we're the sink the source will never be an HGO or HGT. */
		if (link->sink == pad)
			return link->source;

		if (link->source != pad)
			continue;

		/* If the sink isn't a subdevice it can't be an HGO or HGT. */
		if (!is_media_entity_v4l2_subdev(link->sink->entity))
			return link->sink;

		entity = media_entity_to_vsp1_entity(link->sink->entity);
		if (entity->type != VSP1_ENTITY_HGO &&
		    entity->type != VSP1_ENTITY_HGT)
			return link->sink;
	}

	return NULL;

}

/* -----------------------------------------------------------------------------
 * Initialization
 */

#define VSP1_ENTITY_ROUTE(ent)						\
	{ VSP1_ENTITY_##ent, 0, VI6_DPR_##ent##_ROUTE,			\
	  { VI6_DPR_NODE_##ent }, VI6_DPR_NODE_##ent }

#define VSP1_ENTITY_ROUTE_RPF(idx)					\
	{ VSP1_ENTITY_RPF, idx, VI6_DPR_RPF_ROUTE(idx),			\
	  { 0, }, VI6_DPR_NODE_RPF(idx) }

#define VSP1_ENTITY_ROUTE_UDS(idx)					\
	{ VSP1_ENTITY_UDS, idx, VI6_DPR_UDS_ROUTE(idx),			\
	  { VI6_DPR_NODE_UDS(idx) }, VI6_DPR_NODE_UDS(idx) }

#define VSP1_ENTITY_ROUTE_UIF(idx)					\
	{ VSP1_ENTITY_UIF, idx, VI6_DPR_UIF_ROUTE(idx),			\
	  { VI6_DPR_NODE_UIF(idx) }, VI6_DPR_NODE_UIF(idx) }

#define VSP1_ENTITY_ROUTE_WPF(idx)					\
	{ VSP1_ENTITY_WPF, idx, 0,					\
	  { VI6_DPR_NODE_WPF(idx) }, VI6_DPR_NODE_WPF(idx) }

static const struct vsp1_route vsp1_routes[] = {
	{ VSP1_ENTITY_IIF, 0, VI6_DPR_BRU_ROUTE,
	  { VI6_DPR_NODE_BRU_IN(0), VI6_DPR_NODE_BRU_IN(1),
	    VI6_DPR_NODE_BRU_IN(3) }, VI6_DPR_NODE_WPF(0) },
	{ VSP1_ENTITY_BRS, 0, VI6_DPR_ILV_BRS_ROUTE,
	  { VI6_DPR_NODE_BRS_IN(0), VI6_DPR_NODE_BRS_IN(1) }, 0 },
	{ VSP1_ENTITY_BRU, 0, VI6_DPR_BRU_ROUTE,
	  { VI6_DPR_NODE_BRU_IN(0), VI6_DPR_NODE_BRU_IN(1),
	    VI6_DPR_NODE_BRU_IN(2), VI6_DPR_NODE_BRU_IN(3),
	    VI6_DPR_NODE_BRU_IN(4) }, VI6_DPR_NODE_BRU_OUT },
	VSP1_ENTITY_ROUTE(CLU),
	{ VSP1_ENTITY_HGO, 0, 0, { 0, }, 0 },
	{ VSP1_ENTITY_HGT, 0, 0, { 0, }, 0 },
	VSP1_ENTITY_ROUTE(HSI),
	VSP1_ENTITY_ROUTE(HST),
	{ VSP1_ENTITY_LIF, 0, 0, { 0, }, 0 },
	{ VSP1_ENTITY_LIF, 1, 0, { 0, }, 0 },
	VSP1_ENTITY_ROUTE(LUT),
	VSP1_ENTITY_ROUTE_RPF(0),
	VSP1_ENTITY_ROUTE_RPF(1),
	VSP1_ENTITY_ROUTE_RPF(2),
	VSP1_ENTITY_ROUTE_RPF(3),
	VSP1_ENTITY_ROUTE_RPF(4),
	VSP1_ENTITY_ROUTE(SRU),
	VSP1_ENTITY_ROUTE_UDS(0),
	VSP1_ENTITY_ROUTE_UDS(1),
	VSP1_ENTITY_ROUTE_UDS(2),
	VSP1_ENTITY_ROUTE_UIF(0),	/* Named UIF4 in the documentation */
	VSP1_ENTITY_ROUTE_UIF(1),	/* Named UIF5 in the documentation */
	VSP1_ENTITY_ROUTE_WPF(0),
	VSP1_ENTITY_ROUTE_WPF(1),
	VSP1_ENTITY_ROUTE_WPF(2),
	VSP1_ENTITY_ROUTE_WPF(3),
};

int vsp1_entity_init(struct vsp1_device *vsp1, struct vsp1_entity *entity,
		     const char *name, unsigned int num_pads,
		     const struct v4l2_subdev_ops *ops, u32 function)
{
	static struct lock_class_key key;
	struct v4l2_subdev *subdev;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(vsp1_routes); ++i) {
		if (vsp1_routes[i].type == entity->type &&
		    vsp1_routes[i].index == entity->index) {
			entity->route = &vsp1_routes[i];
			break;
		}
	}

	if (i == ARRAY_SIZE(vsp1_routes))
		return -EINVAL;

	mutex_init(&entity->lock);

	entity->vsp1 = vsp1;
	entity->source_pad = num_pads - 1;

	/* Allocate and initialize pads. */
	entity->pads = devm_kcalloc(vsp1->dev,
				    num_pads, sizeof(*entity->pads),
				    GFP_KERNEL);
	if (entity->pads == NULL)
		return -ENOMEM;

	for (i = 0; i < num_pads - 1; ++i)
		entity->pads[i].flags = MEDIA_PAD_FL_SINK;

	entity->sources = devm_kcalloc(vsp1->dev, max(num_pads - 1, 1U),
				       sizeof(*entity->sources), GFP_KERNEL);
	if (entity->sources == NULL)
		return -ENOMEM;

	/* Single-pad entities only have a sink. */
	entity->pads[num_pads - 1].flags = num_pads > 1 ? MEDIA_PAD_FL_SOURCE
					 : MEDIA_PAD_FL_SINK;

	/* Initialize the media entity. */
	ret = media_entity_pads_init(&entity->subdev.entity, num_pads,
				     entity->pads);
	if (ret < 0)
		return ret;

	/* Initialize the V4L2 subdev. */
	subdev = &entity->subdev;
	v4l2_subdev_init(subdev, ops);
	subdev->internal_ops = &vsp1_entity_internal_ops;

	subdev->entity.function = function;
	subdev->entity.ops = &vsp1->media_ops;
	subdev->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;

	snprintf(subdev->name, sizeof(subdev->name), "%s %s",
		 dev_name(vsp1->dev), name);

	vsp1_entity_init_state(subdev, NULL);

	/*
	 * Allocate the subdev state to store formats and selection
	 * rectangles.
	 */
	/*
	 * FIXME: Drop this call, drivers are not supposed to use
	 * __v4l2_subdev_state_alloc().
	 */
	entity->state = __v4l2_subdev_state_alloc(&entity->subdev,
						  "vsp1:state->lock", &key);
	if (IS_ERR(entity->state)) {
		media_entity_cleanup(&entity->subdev.entity);
		return PTR_ERR(entity->state);
	}

	return 0;
}

void vsp1_entity_destroy(struct vsp1_entity *entity)
{
	if (entity->ops && entity->ops->destroy)
		entity->ops->destroy(entity);
	if (entity->subdev.ctrl_handler)
		v4l2_ctrl_handler_free(entity->subdev.ctrl_handler);
	__v4l2_subdev_state_free(entity->state);
	media_entity_cleanup(&entity->subdev.entity);
}
