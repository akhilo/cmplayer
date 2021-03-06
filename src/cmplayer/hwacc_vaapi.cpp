#include "hwacc_vaapi.hpp"
#include "stdafx.hpp"
#include "videoframe.hpp"
#include "openglmisc.hpp"

#ifdef Q_OS_LINUX

extern "C" {
#include <video/mp_image.h>
#include <common/av_common.h>
#include <video/mp_image_pool.h>
#include <va/va_glx.h>
#include <video/sws_utils.h>
#include <libavcodec/vaapi.h>
}

bool VaApi::init = false;
bool VaApi::ok = false;
VADisplay VaApi::m_display = nullptr;
VaApi &VaApi::get() {static VaApi info; return info;}

void HwAccX11Trait<IMGFMT_VAAPI>::destroySurface(SurfaceID id) {
   vaDestroySurfaces(VaApi::glx(), &id, 1);
}

bool HwAccX11Trait<IMGFMT_VAAPI>::createSurfaces(int width, int height, int format, QVector<SurfaceID> &ids) {
	return VaApiStatusChecker().isSuccess(vaCreateSurfaces(VaApi::glx(), width, height, format, ids.size(), ids.data()));
}

#ifdef USE_VAVPP
VAProcDeinterlacingType VaApi::toVAType(DeintMethod method) {
	switch (method) {
	case DeintMethod::Bob:
		return VAProcDeinterlacingBob;
	case DeintMethod::MotionAdaptive:
		return VAProcDeinterlacingMotionAdaptive;
	default:
		return VAProcDeinterlacingNone;
	}
}

QString VaApiFilterInfo::description(VAProcFilterType type, int algorithm) {
	switch (type) {
	case VAProcFilterNoiseReduction:
		return _L("Noise reduction filter");
	case VAProcFilterSharpening:
		return _L("Sharpening filter");
	case VAProcFilterDeinterlacing:
		switch (algorithm) {
		case VAProcDeinterlacingBob:
			return _L("Bob deinterlacer");
		case VAProcDeinterlacingWeave:
			return _L("Weave deinterlacer");
		case VAProcDeinterlacingMotionAdaptive:
			return _L("Motion adaptive deinterlacer");
		case VAProcDeinterlacingMotionCompensated:
			return _L("Motion compensation deinterlacer");
		default:
			break;
		}
	default:
		break;
	}
	return "";
}

QList<int> VaApi::algorithms(VAProcFilterType type) {
	QList<int> ret;
	switch (type) {
	case VAProcFilterNoiseReduction:
	case VAProcFilterSharpening:
		ret << type;
		break;
	case VAProcFilterDeinterlacing:
		for (int i=1; i<VAProcDeinterlacingCount; ++i)
			ret << i;
		break;
	default:
		break;
	}
	return ret;
}

VaApiFilterInfo::VaApiFilterInfo(VAContextID context, VAProcFilterType type) {
	m_type = type;
	uint size = 0;
	auto dpy = VaApi::glx();
	switch (type) {
	case VAProcFilterNoiseReduction:
	case VAProcFilterSharpening: {
		VAProcFilterCap cap; size = 1;
		if (!isSuccess(vaQueryVideoProcFilterCaps(dpy, context, type, &cap, &size)) || size != 1)
			return;
		m_caps.resize(1); m_caps[0].algorithm = type; m_caps[0].range = cap.range;
		break;
	} case VAProcFilterDeinterlacing: {
		size = VAProcDeinterlacingCount;
		VAProcFilterCapDeinterlacing caps[VAProcDeinterlacingCount];
		if (!isSuccess(vaQueryVideoProcFilterCaps(dpy, context, VAProcFilterDeinterlacing, caps, &size)))
			return;
		m_caps.resize(size);
		for (uint i=0; i<size; ++i)
			m_caps[i].algorithm = caps[i].type;
		break;
	} case VAProcFilterColorBalance: {
		size = VAProcColorBalanceCount;
		VAProcFilterCapColorBalance caps[VAProcColorBalanceCount];
		if (!isSuccess(vaQueryVideoProcFilterCaps(dpy, context, VAProcFilterColorBalance, caps, &size)))
			return;
		m_caps.resize(size);
		for (uint i=0; i<size; ++i) {
			m_caps[i].algorithm = caps[i].type;
			m_caps[i].range = caps[i].range;
		}
		break;
	} default:
		return;
	}
	m_algorithms.resize(m_caps.size());
	for (int i=0; i<m_caps.size(); ++i)
		m_algorithms[i] = m_caps[i].algorithm;
}
#endif

VaApi::VaApi() {
	init = true;
	auto xdpy = QX11Info::display();
	VADisplay display = m_display = vaGetDisplayGLX(xdpy);
	if (!check(display ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_UNIMPLEMENTED, "Cannot create VADisplay."))
		return;
	int major, minor;
	if (!check(vaInitialize(m_display, &major, &minor), "Cannot initialize VA-API."))
		return;
	auto size = vaMaxNumProfiles(display);
	m_profiles.resize(size);
	if (!check(vaQueryConfigProfiles(display, m_profiles.data(), &size), "No available profiles."))
		return;
	m_profiles.resize(size);

	for (auto profile : m_profiles) {
		int size = vaMaxNumEntrypoints(display);
        QVector<VAEntrypoint> entries(size);
		if (!isSuccess(vaQueryConfigEntrypoints(display, profile, entries.data(), &size)))
			continue;
		entries.resize(size);
		m_entries.insert(profile, entries);
	}

	initCodecs();
#ifdef USE_VAVPP
	initFilters();
#endif
	_Debug("VA-API is initialized.");
	ok = true;
}

void VaApi::initialize() {
	if (!VaApi::init)
		VaApi::get().glx();
}

void VaApi::finalize() {
	if (!VaApi::init)
		return;
	auto &dpy = VaApi::get().m_display;
	if (dpy) {
		vaTerminate(dpy);
		dpy = nullptr;
	}
	init = false;
}

void VaApi::initCodecs() {
	auto supports = [this](const QVector<VAProfile> &va_all, const QVector<int> &av_all, int surfaces, AVCodecID id) {
		QVector<VAProfile> va; QVector<int> av;
		for (int i=0; i<va_all.size(); ++i) {
			if (hasEntryPoint(VAEntrypointVLD, va_all[i])) {
				va.push_back(va_all[i]);
				av.push_back(av_all[i]);
			}
		}
		if (!va.isEmpty())
			m_supported.insert(id, VaApiCodec(m_profiles, va, av, surfaces + 4, id));
	};
#define NUM_VIDEO_SURFACES_MPEG2  3 /* 1 decode frame, up to  2 references */
#define NUM_VIDEO_SURFACES_MPEG4  3 /* 1 decode frame, up to  2 references */
#define NUM_VIDEO_SURFACES_H264  21 /* 1 decode frame, up to 20 references */
#define NUM_VIDEO_SURFACES_VC1    3 /* 1 decode frame, up to  2 references */
	const QVector<VAProfile> vampeg2s = {VAProfileMPEG2Main, VAProfileMPEG2Simple};
	const QVector<int> avmpeg2s = {FF_PROFILE_MPEG2_MAIN, FF_PROFILE_MPEG2_SIMPLE};
	const QVector<VAProfile> vampeg4s = {VAProfileMPEG4Main, VAProfileMPEG4AdvancedSimple, VAProfileMPEG4Simple};
	const QVector<int> avmpeg4s = {FF_PROFILE_MPEG4_MAIN, FF_PROFILE_MPEG4_ADVANCED_SIMPLE, FF_PROFILE_MPEG4_SIMPLE};
	const QVector<VAProfile> vah264s = {VAProfileH264High, VAProfileH264Main, VAProfileH264Baseline, VAProfileH264ConstrainedBaseline};
	const QVector<int> avh264s = {FF_PROFILE_H264_HIGH, FF_PROFILE_H264_MAIN, FF_PROFILE_H264_BASELINE, FF_PROFILE_H264_CONSTRAINED_BASELINE};
	const QVector<VAProfile> vawmv3s = {VAProfileVC1Main, VAProfileVC1Simple, VAProfileVC1Advanced};
	const QVector<int> avwmv3s = {FF_PROFILE_VC1_MAIN, FF_PROFILE_VC1_SIMPLE, FF_PROFILE_VC1_ADVANCED};
	const QVector<VAProfile> vavc1s = {VAProfileVC1Advanced, VAProfileVC1Main, VAProfileVC1Simple};
	const QVector<int> avvc1s = {FF_PROFILE_VC1_ADVANCED, FF_PROFILE_VC1_MAIN, FF_PROFILE_VC1_SIMPLE};
	supports(vampeg2s, avmpeg2s, NUM_VIDEO_SURFACES_MPEG2, AV_CODEC_ID_MPEG1VIDEO);
	supports(vampeg2s, avmpeg2s, NUM_VIDEO_SURFACES_MPEG2, AV_CODEC_ID_MPEG2VIDEO);
	supports(vampeg4s, avmpeg4s, NUM_VIDEO_SURFACES_MPEG4, AV_CODEC_ID_MPEG4);
	supports(vawmv3s, avwmv3s, NUM_VIDEO_SURFACES_VC1, AV_CODEC_ID_WMV3);
	supports(vavc1s, avvc1s, NUM_VIDEO_SURFACES_VC1, AV_CODEC_ID_VC1);
	supports(vah264s, avh264s, NUM_VIDEO_SURFACES_H264, AV_CODEC_ID_H264);
}

#ifdef USE_VAVPP
void VaApi::initFilters() {
	if (!hasEntryPoint(VAEntrypointVideoProc, VAProfileNone))
		return;
	auto display = VaApi::glx();
	VAConfigID config = VA_INVALID_ID;
	VAContextID context = VA_INVALID_ID;
	do {
		if (!isSuccess(vaCreateConfig(display, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &config)))
			break;
		if (!isSuccess(vaCreateContext(display, config, 0, 0, 0, nullptr, 0, &context)))
			break;
		QVector<VAProcFilterType> types(VAProcFilterCount);
		uint size = VAProcFilterCount;
		if (!isSuccess(vaQueryVideoProcFilters(display, context, types.data(), &size)))
			break;
		types.resize(size);
		for (const auto &type : types) {
			VaApiFilterInfo info(context, type);
			if (info.isSuccess() && !info.algorithms().isEmpty())
				m_filters.insert(type, info);
		}
	} while (false);
	if (context != VA_INVALID_ID)
		vaDestroyContext(display, context);
	if (config != VA_INVALID_ID)
		vaDestroyConfig(display, config);
}
#endif

int VaApi::toVAType(int mp_fields, bool first) {
	static const int field[] = {VA_BOTTOM_FIELD, VA_TOP_FIELD};
	return field[(!(mp_fields & MP_IMGFIELD_TOP_FIRST)) ^ (int)first];
}

/***************************************************************************************************/

struct HwAccVaApi::Data {
	vaapi_context context = {nullptr, VA_INVALID_ID, VA_INVALID_ID, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	VAProfile profile = VAProfileNone;
	VaApiSurfacePool pool;
};

HwAccVaApi::HwAccVaApi(AVCodecID cid)
: HwAcc(cid), d(new Data) {
}

HwAccVaApi::~HwAccVaApi() {
	freeContext();
	if (d->context.config_id != VA_INVALID_ID)
		vaDestroyConfig(d->context.display, d->context.config_id);
	delete d;
}

bool HwAccVaApi::isOk() const {
	return status() == VA_STATUS_SUCCESS;
}

void *HwAccVaApi::context() const {
	return &d->context;
}

mp_image *HwAccVaApi::getSurface() {
	return d->pool.getMpImage();
}

void HwAccVaApi::freeContext() {
	if (d->context.display) {
		if (d->context.context_id != VA_INVALID_ID)
			vaDestroyContext(d->context.display, d->context.context_id);
	}
	d->context.context_id = VA_INVALID_ID;
}

bool HwAccVaApi::fillContext(AVCodecContext *avctx) {
	if (status() != VA_STATUS_SUCCESS)
		return false;
	freeContext();
	d->context.display = VaApi::glx();
	if (!d->context.display)
		return false;
	const auto codec = VaApi::codec(avctx->codec_id);
	if (!codec)
		return false;
	d->profile = codec->profile(avctx->profile);
	VAConfigAttrib attr = { VAConfigAttribRTFormat, 0 };
	if(!isSuccess(vaGetConfigAttributes(d->context.display, d->profile, VAEntrypointVLD, &attr, 1)))
		return false;
	const uint rts =  attr.value & (VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV422);
	if(!rts)
		return isSuccess(VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
	if(!isSuccess(vaCreateConfig(d->context.display, d->profile, VAEntrypointVLD, &attr, 1, &d->context.config_id)))
		return false;
	const int w = avctx->width, h = avctx->height;
	auto tryRtFormat = [rts, this, codec, w, h] (uint rt) { return (rts & rt) && d->pool.create(codec->surfaces, w, h, rt); };
	if (!tryRtFormat(VA_RT_FORMAT_YUV420) && !tryRtFormat(VA_RT_FORMAT_YUV422))
		return false;
	VaApi::get().setSurfaceFormat(d->pool.format());
	auto ids = d->pool.ids();
	if (!isSuccess(vaCreateContext(d->context.display, d->context.config_id, w, h, VA_PROGRESSIVE, ids.data(), ids.size(), &d->context.context_id)))
		return false;
	return true;
}

mp_image *HwAccVaApi::getImage(mp_image *mpi) {
	return mpi;
}

/****************************************************************************************/

VaApiMixer::VaApiMixer(const QList<OpenGLTexture2D> &textures, const VideoFormat &/*format*/) {
	Q_ASSERT(textures.size() == 1);
	const auto &texture = textures.first();
	texture.bind();
	if (!check(vaCreateSurfaceGLX(VaApi::glx(), texture.target(), texture.id(), &m_glSurface), "Cannot create OpenGL surface."))
		return;
}

VaApiMixer::~VaApiMixer() {
	if (m_glSurface)
		vaDestroySurfaceGLX(VaApi::glx(), m_glSurface);
}

void VaApiMixer::adjust(VideoFormatData *data, const mp_image *mpi) {
	Q_ASSERT(data->imgfmt == IMGFMT_VAAPI);
	data->type = IMGFMT_BGRA;
	data->planes = 1;
	const int stride = FFALIGN((mpi->w * 32 + 7) / 8, 16);
	data->alignedSize = QSize(stride/4, mpi->h);
	data->alignedByteSize[0] = QSize(stride, mpi->h);
	data->bpp = 32;
	data->colorspace = MP_CSP_RGB;
}

bool VaApiMixer::upload(const VideoFrame &frame, bool deint) {
	if (!m_glSurface)
		return false;
	static const int specs[MP_CSP_COUNT] = {
		0,					//MP_CSP_AUTO,
		VA_SRC_BT601,		//MP_CSP_BT_601,
		VA_SRC_BT709,		//MP_CSP_BT_709,
		VA_SRC_SMPTE_240,	//MP_CSP_SMPTE_240M,
		0,					//MP_CSP_RGB,
		0,					//MP_CSP_XYZ,
		0,					//MP_CSP_YCGCO,
	};
	static const int field[] = {
		// Picture = 0,   Top = 1,      Bottom = 2
		VA_FRAME_PICTURE, VA_TOP_FIELD, VA_BOTTOM_FIELD, VA_FRAME_PICTURE
	};
	const auto id = (VASurfaceID)(quintptr)frame.data(3);
	int flags = specs[frame.format().colorspace()];
	if (deint)
		flags |= field[frame.field() & VideoFrame::Interlaced];
	if (!check(vaCopySurfaceGLX(VaApi::glx(), m_glSurface, id,  flags), "Cannot copy OpenGL surface."))
		return false;
	if (!check(vaSyncSurface(VaApi::glx(), id), "Cannot sync video surface."))
		return false;
	return true;
}

#endif
