

// Platform-specific only
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dxgi1_6.h>
#include <d2d1_1.h>
#include <DirectXPackedVector.h>
#include <atlbase.h>
#include <comdef.h>

#include "grabber/dda/DDAGrabber.h"
#include <QDebug>
#include <QJsonDocument>
#include <QElapsedTimer>

#include <utils/Logger.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

// Constants
namespace {
	constexpr std::chrono::milliseconds RETRY_INTERVAL{ 2000 };

	// HDR handling
	// scRGB (FP16) desktop: a value of 1.0 equals 80 nits
	constexpr float SCRGB_REFERENCE_NITS = 80.0F;
	// Used, if the Windows "SDR content brightness" cannot be queried
	constexpr float DEFAULT_SDR_WHITE_NITS = 200.0F;
	// Re-read the SDR white level periodically, as the user can change it at any time
	constexpr std::chrono::milliseconds SDR_WHITE_LEVEL_REFRESH_INTERVAL{ 5000 };
	// Resolution of the linear -> sRGB lookup table
	constexpr int SRGB_LUT_SIZE = 16384;

	///
	/// @brief Query the Windows "SDR content brightness" (SDR white level) of an HDR display.
	///
	/// @param[in] gdiDeviceName GDI device name of the output, e.g. "\\.\DISPLAY1"
	/// @param[in] fallbackNits  Value returned, if the level cannot be determined
	/// @return SDR white level in nits
	///
	float querySdrWhiteLevelNits(const std::wstring& gdiDeviceName, float fallbackNits)
	{
		UINT32 pathCount = 0;
		UINT32 modeCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
		{
			return fallbackNits;
		}

		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
		{
			return fallbackNits;
		}
		paths.resize(pathCount);

		for (const auto& path : paths)
		{
			DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
			sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = path.sourceInfo.adapterId;
			sourceName.header.id = path.sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS)
			{
				continue;
			}

			if (gdiDeviceName != sourceName.viewGdiDeviceName)
			{
				continue;
			}

			DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
			whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
			whiteLevel.header.size = sizeof(whiteLevel);
			whiteLevel.header.adapterId = path.targetInfo.adapterId;
			whiteLevel.header.id = path.targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS && whiteLevel.SDRWhiteLevel > 0)
			{
				// SDRWhiteLevel is a multiplier of 80 nits, scaled by 1000
				return static_cast<float>(whiteLevel.SDRWhiteLevel) / 1000.0F * SCRGB_REFERENCE_NITS;
			}
		}
		return fallbackNits;
	}

	///
	/// @brief Build a lookup table mapping linear light [0..1] to 8-bit sRGB
	///
	std::vector<uint8_t> buildLinearToSrgbLut()
	{
		std::vector<uint8_t> lut(SRGB_LUT_SIZE + 1);
		for (int i = 0; i <= SRGB_LUT_SIZE; ++i)
		{
			const double linear = static_cast<double>(i) / SRGB_LUT_SIZE;
			const double srgb = (linear <= 0.0031308)
				? linear * 12.92
				: 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
			lut[i] = static_cast<uint8_t>(std::lround(std::clamp(srgb, 0.0, 1.0) * 255.0));
		}
		return lut;
	}
} //End of constants

// Logs a message along with the hex error HRESULT.
#define LOG_ERROR(hr, msg) Error(_log, msg ": 0x%x", hr)

// Checks if the HRESULT is an error, and if so, logs it and returns from the
// current function.
#define RETURN_IF_ERROR(hr, msg, returnValue)                                                                          \
	if (FAILED(hr))                                                                                                    \
	{                                                                                                                  \
		setInError(QString("%1: 0x%2").arg(msg).arg(hr));                                                              \
		return returnValue;                                                                                            \
	}

// Checks if the condition is false, and if so, logs an error and returns from
// the current function.
#define RET_CHECK(cond, returnValue)                                                                                   \
	if (!(cond))                                                                                                       \
	{                                                                                                                  \
		Error(_log, "Assertion failed: " #cond);                                                                       \
		return returnValue;                                                                                            \
	}

// Safely call IDXGIOutputDuplication::ReleaseFrame and swallow SEH that some drivers raise
static inline HRESULT SafeReleaseFrame(IDXGIOutputDuplication* dup)
{
	if (!dup) return E_POINTER;
#if defined(_MSC_VER)
	__try
	{
		return dup->ReleaseFrame();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return DXGI_ERROR_INVALID_CALL;
	}
#else
	return dup->ReleaseFrame();
#endif
}

class DDAGrabberImpl
{
public:
	CComPtr<ID3D11Device> device;
	CComPtr<ID3D11DeviceContext> deviceContext;
	CComPtr<IDXGIDevice> dxgiDevice;
	CComPtr<IDXGIAdapter> dxgiAdapter;
	CComPtr<IDXGIOutput1> dxgiOutput1;
	CComPtr<IDXGIOutputDuplication> desktopDuplication;

	CComPtr<ID3D11Texture2D> intermediateTexture;

	CComPtr<ID2D1Factory1>        d2dFactory;
	CComPtr<ID2D1Device>          d2dDevice;
	CComPtr<ID2D1DeviceContext>   d2dContext;
	CComPtr<ID3D11Texture2D>      d2dConvertedTexture; // Holds the B8G8R8A8 converted image

	// Pre-calculated members for performance
	CComPtr<ID2D1Bitmap1>     destBitmap;
	D2D1_MATRIX_3X2_F         orientationTransform{};
	D2D1_RECT_F               sourceRect{};
	D2D1_RECT_F               destRect{};

	int display = 0;

	DXGI_MODE_ROTATION desktopRotation = DXGI_MODE_ROTATION_IDENTITY;
	int desktopWidth = 0;
	int desktopHeight = 0;
	bool isFrameAcquired = false;

	// Retry timer for reinitializing duplication when unavailable/access denied
	QElapsedTimer lastRetryTimer;

	// HDR handling
	std::wstring gdiDeviceName;                                  // e.g. "\\.\DISPLAY1"
	bool isHdr = false;                                          // Output is in HDR (PQ / BT.2020) mode
	float sdrWhiteNits = DEFAULT_SDR_WHITE_NITS;                 // Windows "SDR content brightness"
	QElapsedTimer sdrWhiteLevelTimer;
	DXGI_FORMAT pipelineFormat = DXGI_FORMAT_B8G8R8A8_UNORM;     // Format of the D2D target / staging textures
	std::vector<uint8_t> linearToSrgbLut;

	// Fallback, if D2D cannot read the duplicated texture directly (e.g. not bindable as shader resource)
	CComPtr<ID3D11Texture2D> sourceCopyTexture;
	bool useSourceCopy = false;
};

DDAGrabber::DDAGrabber(int display, int cropLeft, int cropRight, int cropTop, int cropBottom)
	: Grabber("GRABBER-DDA", cropLeft, cropRight, cropTop, cropBottom)
	, d(std::make_unique<DDAGrabberImpl>())
{
	TRACK_SCOPE() << "Creating DDA grabber for display" << d->display;
	_useImageResampler = false;
	d->display = display;
	// Initialize retry timer
	d->lastRetryTimer.start();
	d->sdrWhiteLevelTimer.start();
	d->linearToSrgbLut = buildLinearToSrgbLut();
}

DDAGrabber::~DDAGrabber()
{
	TRACK_SCOPE();
	if (d->isFrameAcquired && d->desktopDuplication)
	{
		SafeReleaseFrame(d->desktopDuplication);
		d->isFrameAcquired = false;
	}
}

void DDAGrabber::validateCursorSettings()
{
	// 1. Check for Mouse Trails (The most common culprit for "baked-in" cursors)
	int trailLines = 0;
	if (SystemParametersInfo(SPI_GETMOUSETRAILS, 0, &trailLines, 0))
	{
		if (trailLines > 1)
		{
			Warning(_log, "Incompatible Setting Detected: 'Display pointer trails' is ENABLED. "
				"A mouse cursor will appear in your screen capture. Disable this in Windows Mouse settings.");
		}
	}

	// 2. Check for Cursor Shadow (Can also trigger software compositing on older drivers/hardware)
	BOOL shadowEnabled = FALSE;
	if (SystemParametersInfo(SPI_GETCURSORSHADOW, 0, &shadowEnabled, 0))
	{
		if (shadowEnabled)
		{
			Info(_log, "Note: 'Pointer shadow' is enabled. If a mouse cursor appears in capture, "
				"try disabling this setting in Windows.");
		}
	}
}

bool DDAGrabber::setupDisplay()
{
	qCDebug(grabber_screen_flow) << "Setting up DDA grabber for display" << d->display;

	d->device.Release();
	d->deviceContext.Release();
	d->dxgiDevice.Release();
	d->dxgiAdapter.Release();
	d->dxgiOutput1.Release();
	d->desktopDuplication.Release();

	// Release D2D resources
	d->d2dFactory.Release();
	d->d2dDevice.Release();
	d->d2dContext.Release();
	d->d2dConvertedTexture.Release();
	d->destBitmap.Release();

	// Create a hardware device only (Desktop Duplication requires hardware adapter)
	UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

	// If you want D3D debug messages, you can add the debug flag too
#ifdef QT_DEBUG
	createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	HRESULT hr{ S_OK };
	hr = D3D11CreateDevice(
		nullptr,                 // Default adapter
		D3D_DRIVER_TYPE_HARDWARE,// Require hardware for DDA
		nullptr,                 // Software device (not used)
		createFlags,             // Flags
		nullptr,                 // Feature levels to attempt
		0,                       // Number of feature levels
		D3D11_SDK_VERSION,       // SDK version
		&d->device,              // Returned ID3D11Device
		nullptr,                 // Returned feature level
		&d->deviceContext        // Returned ID3D11DeviceContext
	);
	RETURN_IF_ERROR(hr, "CreateDevice failed", false);

	hr = d->device->QueryInterface(&d->dxgiDevice);
	RETURN_IF_ERROR(hr, "Failed to get DXGI device", false);

	D2D1_FACTORY_OPTIONS d2dOptions = {};
#ifdef QT_DEBUG
	d2dOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
	hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &d2dOptions, (void**)&d->d2dFactory);
	RETURN_IF_ERROR(hr, "Failed to create D2D1Factory", false);

	hr = d->d2dFactory->CreateDevice(d->dxgiDevice, &d->d2dDevice);
	RETURN_IF_ERROR(hr, "Failed to create D2D1Device", false);

	hr = d->d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d->d2dContext);
	RETURN_IF_ERROR(hr, "Failed to create D2D1DeviceContext", false);

	hr = d->dxgiDevice->GetAdapter(&d->dxgiAdapter);
	RETURN_IF_ERROR(hr, "Failed to get DXGI adapter", false);

	// Log adapter description and LUID for diagnostics
	if (grabber_screen_flow().isDebugEnabled())
	{
		DXGI_ADAPTER_DESC adapterDesc{};
		d->dxgiAdapter->GetDesc(&adapterDesc);
		qCDebug(grabber_screen_flow) << "DXGI Adapter:" << QString::fromWCharArray(adapterDesc.Description)
			<< ", VendorId:" << adapterDesc.VendorId << ", DeviceId:" << adapterDesc.DeviceId
			<< ", SubSysId:" << adapterDesc.SubSysId << ", Revision:" << adapterDesc.Revision
			<< ", LUID:" << (qulonglong)adapterDesc.AdapterLuid.HighPart << ":" << (qulonglong)adapterDesc.AdapterLuid.LowPart;

		// Log session info (console session id)
		DWORD sessionId = WTSGetActiveConsoleSessionId();
		qCDebug(grabber_screen_flow) << "Active console session id:" << sessionId;
	}

	return true;
}

bool DDAGrabber::restartCapture()
{
	if (_isDeviceInError)
	{
		Error(_log, "Cannot restart capture, device is in error state");
		return false;
	}

	validateCursorSettings();

	qCDebug(grabber_screen_flow) << "Restarting capture for display" << d->display;

	if (d->dxgiAdapter == nullptr)
	{
		if (!setupDisplay())
		{
			setInError("restartCapture - Setting up the display failed");
			return false;
		}
		// Continue with capture setup after setting up the display successfully
	}

	HRESULT hr{ S_OK };

	CComPtr<IDXGIOutput> output;
	hr = d->dxgiAdapter->EnumOutputs(d->display, &output);
	RETURN_IF_ERROR(hr, "Failed to get output", false);

	// Forcefully drop previous duplication state after ACCESS_LOST/SESSION_DISCONNECTED
	// so we never "reuse" an invalid duplication/output.
	d->desktopDuplication.Release();
	d->dxgiOutput1.Release();

	// Always re-query a fresh IDXGIOutput1 from the current output
	{
		qCDebug(grabber_screen_flow) << "Creating new output for display" << d->display;
		CComPtr<IDXGIOutput1> output1;
		hr = output->QueryInterface(&output1);
		RETURN_IF_ERROR(hr, "Failed to get output1", false);
		d->dxgiOutput1 = output1;
		qCDebug(grabber_screen_flow) << "Created new output for display" << d->display;
	}

	DXGI_OUTPUT_DESC desc;
	hr = output->GetDesc(&desc);
	RETURN_IF_ERROR(hr, "Failed to get output description", false);

	d->desktopWidth = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
	d->desktopHeight = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
	d->gdiDeviceName = desc.DeviceName;

	// Detect, if the output runs in HDR mode (Windows "Use HDR" enabled)
	d->isHdr = false;
	{
		CComPtr<IDXGIOutput6> output6;
		if (SUCCEEDED(output->QueryInterface(&output6)))
		{
			DXGI_OUTPUT_DESC1 desc1{};
			if (SUCCEEDED(output6->GetDesc1(&desc1)))
			{
				d->isHdr = (desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
			}
		}
	}
	if (d->isHdr)
	{
		d->sdrWhiteNits = querySdrWhiteLevelNits(d->gdiDeviceName, DEFAULT_SDR_WHITE_NITS);
		d->sdrWhiteLevelTimer.restart();
		Info(_log, "Display %d is in HDR mode. Capturing in FP16 (scRGB), SDR white level: %.0f nits", d->display, d->sdrWhiteNits);
	}

	// Creates the desktop duplication.
	// In HDR mode the frames are requested as FP16 scRGB, so that they are always delivered in one consistent,
	// linear format. The legacy DuplicateOutput lets Windows convert HDR to 8-bit, which is not consistent
	// for all frames (e.g. partial updates from non-focused windows result in heavily dimmed frames).
	bool isFp16Duplication = false;
	auto duplicateOutput = [this, &isFp16Duplication]() -> HRESULT
	{
		isFp16Duplication = false;
		if (d->isHdr)
		{
			CComPtr<IDXGIOutput5> output5;
			if (SUCCEEDED(d->dxgiOutput1->QueryInterface(&output5)))
			{
				const DXGI_FORMAT supportedFormats[] = { DXGI_FORMAT_R16G16B16A16_FLOAT };
				HRESULT hrFp16 = output5->DuplicateOutput1(d->device, 0, static_cast<UINT>(std::size(supportedFormats)), supportedFormats, &d->desktopDuplication);
				if (SUCCEEDED(hrFp16))
				{
					isFp16Duplication = true;
					return hrFp16;
				}
				Warning(_log, "DuplicateOutput1 (FP16) failed: 0x%x - falling back to legacy capture. HDR colors may be wrong.", hrFp16);
				d->desktopDuplication.Release();
			}
		}
		return d->dxgiOutput1->DuplicateOutput(d->device, &d->desktopDuplication);
	};

	if (_cropLeft + _cropRight >= d->desktopWidth || _cropTop + _cropBottom >= d->desktopHeight)
	{
		Error(_log, "Invalid cropping values which exceed the screen size. Cropping disabled.");
		_cropLeft = _cropRight = _cropTop = _cropBottom = 0;
	}

	int croppedWidth = d->desktopWidth - (_cropLeft + _cropRight);
	int croppedHeight = d->desktopHeight - (_cropTop + _cropBottom);

	int finalWidth = qMax(1, croppedWidth / _pixelDecimation);
	int	finalHeight = qMax(1, croppedHeight / _pixelDecimation);

	// Now duplication is null, so this block will recreate the duplication (and resources if needed)
	if (desc.Rotation != d->desktopRotation || finalWidth != _width || finalHeight != _height || d->desktopDuplication == nullptr)
	{
		Debug(_log, "New capture size or rotation detected. Creating Desktop Duplication for display %d", d->display);

		_width = finalWidth;
		_height = finalHeight;
		d->desktopRotation = desc.Rotation;

		// Recreate desktop duplication
		hr = duplicateOutput();
		if (FAILED(hr))
		{
			LOG_ERROR(hr, "DuplicateOutput failed");
			// If device is lost/removed, recreate device and try again once
			if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
			{
				qCDebug(grabber_screen_flow) << "Device removed/reset. Recreating D3D device and retrying duplication.";
				if (!setupDisplay())
				{
					RETURN_IF_ERROR(hr, "Failed to recreate device after removal", false);
				}
				// Need to re-enum output after device recreation
				CComPtr<IDXGIOutput> outputRetry;
				hr = d->dxgiAdapter->EnumOutputs(d->display, &outputRetry);
				RETURN_IF_ERROR(hr, "Failed to get output (retry)", false);
				CComPtr<IDXGIOutput1> output1Retry;
				hr = outputRetry->QueryInterface(&output1Retry);
				RETURN_IF_ERROR(hr, "Failed to get output1 (retry)", false);
				d->dxgiOutput1 = output1Retry;
				// Retry DuplicateOutput
				hr = duplicateOutput();
			}

			if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
			{
				Error(_log, "DuplicateOutput failed: Another application or system component is currently using the desktop interface.");
			}
			else if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_ACCESS_DENIED)
			{
				// When process runs elevated or policy denies DDA, avoid entering permanent error state.
				Error(_log, "DuplicateOutput failed: Access denied to desktop duplication. Will retry later.");
				return false;
			}
			else if (hr == DXGI_ERROR_SESSION_DISCONNECTED)
			{
				Error(_log, "DuplicateOutput failed: Desktop session is currently locked or disconnected. Will retry.");
			}

			RETURN_IF_ERROR(hr, "Failed to create desktop duplication interface", false);
		}

		// 1. + 2. Create the final GPU texture, the staging texture and the destination D2D bitmap.
		if (!createPipelineResources(isFp16Duplication ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM))
		{
			return false;
		}

		// Determine rotated dimensions for D3D
		bool swapDimensions = d->desktopRotation == DXGI_MODE_ROTATION_ROTATE90 || d->desktopRotation == DXGI_MODE_ROTATION_ROTATE270;
		// This should reference the final output size, not the full desktop size
		int renderTargetWidth = swapDimensions ? _height : _width;
		int renderTargetHeight = swapDimensions ? _width : _height;

		// 3. Pre-calculate the transformation matrix for D2D
		switch (d->desktopRotation)
		{
		case DXGI_MODE_ROTATION_ROTATE90:
			d->orientationTransform =
				D2D1::Matrix3x2F::Rotation(90.0f) *
				D2D1::Matrix3x2F::Translation(static_cast<float>(renderTargetHeight), 0.0f);
			break;
		case DXGI_MODE_ROTATION_ROTATE180:
			d->orientationTransform =
				D2D1::Matrix3x2F::Rotation(180.0f) *
				D2D1::Matrix3x2F::Translation(static_cast<float>(renderTargetWidth), static_cast<float>(renderTargetHeight));
			break;
		case DXGI_MODE_ROTATION_ROTATE270:
			d->orientationTransform =
				D2D1::Matrix3x2F::Rotation(270.0f) *
				D2D1::Matrix3x2F::Translation(0.0f, static_cast<float>(renderTargetWidth));
			break;
		default:
			d->orientationTransform = D2D1::Matrix3x2F::Identity();
			break;
		}

		// 4. Pre-calculate crop and destination rectangles

		// Source D3D picture captured is always not rotated
		int sourceWidth = swapDimensions ? d->desktopHeight : d->desktopWidth;
		int sourceHeight = swapDimensions ? d->desktopWidth : d->desktopHeight;

		D3D11_BOX cropBox{};
		computeCropBox(sourceWidth, sourceHeight, cropBox);

		d->sourceRect = D2D1::RectF(
			static_cast<float>(cropBox.left),
			static_cast<float>(cropBox.top),
			static_cast<float>(cropBox.right),
			static_cast<float>(cropBox.bottom)
		);

		d->destRect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(renderTargetWidth), static_cast<float>(renderTargetHeight));

		Debug(_log, "Display capture set up - Desktop size: %dx%d, Rotation=%d, cropping=%d,%d,%d,%d, decimation=%d, output image size=%dx%d",
			d->desktopWidth, d->desktopHeight, d->desktopRotation, _cropLeft, _cropTop, _cropRight, _cropBottom, _pixelDecimation,
			_width, _height);
	}
	else
	{
		// Should not happen since we forced Release(), but keep fallback log
		qCDebug(grabber_screen_flow) << "Reusing existing output for display" << d->display;
	}

	return true;
}

bool DDAGrabber::createPipelineResources(DXGI_FORMAT format)
{
	qCDebug(grabber_screen_flow) << "Creating final-sized GPU resources [" << _width << "x" << _height << "], format:" << static_cast<int>(format);

	HRESULT hr{ S_OK };

	D3D11_TEXTURE2D_DESC finalDesc = {};
	finalDesc.Width = _width;
	finalDesc.Height = _height;
	finalDesc.Format = format;
	finalDesc.MipLevels = 1;
	finalDesc.ArraySize = 1;
	finalDesc.SampleDesc.Count = 1;
	finalDesc.Usage = D3D11_USAGE_DEFAULT;
	finalDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

	d->d2dConvertedTexture.Release();
	hr = d->device->CreateTexture2D(&finalDesc, nullptr, &d->d2dConvertedTexture);
	RETURN_IF_ERROR(hr, "Failed to create final GPU texture", false);

	// Recreate the CPU staging texture to match
	finalDesc.Usage = D3D11_USAGE_STAGING;
	finalDesc.BindFlags = 0;
	finalDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d->intermediateTexture.Release();
	hr = d->device->CreateTexture2D(&finalDesc, nullptr, &d->intermediateTexture);
	RETURN_IF_ERROR(hr, "Failed to create final staging texture", false);

	// Pre-create the destination D2D bitmap
	CComPtr<IDXGISurface> destSurface;
	hr = d->d2dConvertedTexture->QueryInterface(&destSurface);
	RETURN_IF_ERROR(hr, "Failed to get destination surface", false);
	d->destBitmap.Release();
	if (format == DXGI_FORMAT_B8G8R8A8_UNORM)
	{
		hr = d->d2dContext->CreateBitmapFromDxgiSurface(destSurface, nullptr, &d->destBitmap);
	}
	else
	{
		// FP16 target: D2D draws in linear light, the scRGB values are kept as they are (no clamping)
		const D2D1_BITMAP_PROPERTIES1 targetProperties = D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));
		hr = d->d2dContext->CreateBitmapFromDxgiSurface(destSurface, &targetProperties, &d->destBitmap);
	}
	RETURN_IF_ERROR(hr, "Failed to create destination bitmap", false);

	d->pipelineFormat = format;
	// Re-evaluate, if D2D can read the duplicated frames directly
	d->sourceCopyTexture.Release();
	d->useSourceCopy = false;
	return true;
}

bool DDAGrabber::resetDeviceAndCapture()
{
	qCDebug(grabber_screen_flow) << "Resetting device and capture for display" << d->display;
	resetInError();
	return setupDisplay() && restartCapture();
}

int DDAGrabber::grabFrame(Image<ColorRgb>& image, bool /*forceUpdate*/)
{
	if (_isDeviceInError)
	{
		Error(_log, "Cannot grab frame, device is in error state");
		return -1;
	}

	if (!_isEnabled)
	{
		qCDebug(grabber_screen_capture) << "Capture is disabled";
		return -1;
	}

	if (!d->desktopDuplication)
	{
		qCDebug(grabber_screen_capture) << "Desktop duplication not available (yet)";
		// Try to re-establish duplication periodically
		if (d->lastRetryTimer.hasExpired(RETRY_INTERVAL.count()))
		{
			qCDebug(grabber_screen_flow) << "Attempting to re-establish desktop duplication";
			restartCapture();
			d->lastRetryTimer.restart();
		}
		return -1;
	}

	if (d->isFrameAcquired)
	{
		HRESULT hrRelease = SafeReleaseFrame(d->desktopDuplication);
		d->isFrameAcquired = false;
		if (FAILED(hrRelease))
		{
			if (grabber_screen_capture().isDebugEnabled())
			{
				if (hrRelease == DXGI_ERROR_ACCESS_LOST)
				{
					qCDebug(grabber_screen_capture) << "Failed to release frame- DXGI_ERROR_ACCESS_LOST";
				}
			}
			// Even if release fails, we try to continue. The state is reset.
		}
	}

	HRESULT hr{ S_OK };
	CComPtr<IDXGIResource> desktopResource;
	DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
	hr = d->desktopDuplication->AcquireNextFrame(20, &frameInfo, &desktopResource);
	if (SUCCEEDED(hr))
	{
		d->isFrameAcquired = true;
		// If no pixels changed, skip immediately. This handles the "Only Mouse Moved" case
		if (frameInfo.AccumulatedFrames == 0) {
			SafeReleaseFrame(d->desktopDuplication);
			d->isFrameAcquired = false;
			return -1;
		}
	}

	// --- Error Handling ---
	if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_SESSION_DISCONNECTED || hr == DXGI_ERROR_ACCESS_DENIED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
	{
		if (d->isFrameAcquired)
		{
			SafeReleaseFrame(d->desktopDuplication);
			d->isFrameAcquired = false;
		}

		QString errorText;
		switch (hr)
		{
		case DXGI_ERROR_ACCESS_LOST:
			// Happens, if the graphics device is removed/reset or orientation changed
			qCDebug(grabber_screen_flow) << "Access lost - DXGI_ERROR_ACCESS_LOST. Restarting capture.";
			Error(_log, "Access lost to desktop duplication, attempting to reset device and capture... (DXGI_ERROR_ACCESS_LOST)");
			restartCapture();
			return -1;

			break;
		case DXGI_ERROR_INVALID_CALL:
			// Happens, if a screen is locked or disconnected
			qCDebug(grabber_screen_flow) << "Access lost - DXGI_ERROR_INVALID_CALL";
			restartCapture();
			return -1;
			break;
		case DXGI_ERROR_ACCESS_DENIED:
			errorText = "Access denied : The application does not have the required permissions to capture the desktop";
			break;
		case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
			errorText = "Access lost: Another application or system component is currently using the desktop interface.";
			break;
		case DXGI_ERROR_SESSION_DISCONNECTED:
			errorText = "Access lost: Desktop session is currently locked or disconnected. Will retry.";
			break;
		default:
			errorText = "Access lost : Unknown error";
		}

		if (grabber_screen_flow().isDebugEnabled())
		{
			// Log session/desktop info for diagnostics
			DWORD sessionId = WTSGetActiveConsoleSessionId();
			qCDebug(grabber_screen_flow) << "Active console session id:" << sessionId;
			DXGI_OUTDUPL_DESC duplDesc{};
			if (d->desktopDuplication)
			{
				d->desktopDuplication->GetDesc(&duplDesc);
				qCDebug(grabber_screen_flow) << "Duplication Desc: ModeDesc(" << duplDesc.ModeDesc.Width << "x" << duplDesc.ModeDesc.Height << ") Rot=" << duplDesc.Rotation;
			}
		}

		// For access denied scenarios, do not mark device in error; just return and retry later
		if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_ACCESS_DENIED)
		{
			Error(_log, "%s", errorText.toUtf8().constData());
			// Periodically attempt to recreate duplication
			if (d->lastRetryTimer.hasExpired(RETRY_INTERVAL.count()))
			{
				qCDebug(grabber_screen_flow) << "Access denied, retrying to create duplication later";
				restartCapture();
				d->lastRetryTimer.restart();
			}
			return -1;
		}
		RETURN_IF_ERROR(hr, errorText, -1);
	}

	if (hr == DXGI_ERROR_WAIT_TIMEOUT)
	{
		return -1;
	}

	if (FAILED(hr))
	{
		qCDebug(grabber_screen_flow) << "Failed to acquire next frame: " << hr;
		return -1;
	}

	qCDebug(grabber_screen_capture) << "FrameInfo: Accumulated=" << frameInfo.AccumulatedFrames
		<< ", PointerVisible=" << (int)frameInfo.PointerPosition.Visible
		<< ", LastPresentTime=" << (qulonglong)frameInfo.LastPresentTime.QuadPart
		<< ", LastMouseUpdateTime=" << (qulonglong)frameInfo.LastMouseUpdateTime.QuadPart
		<< " Capture setup: width:" << _width << ", height:" << _height << ", pixelDecimation:" << _pixelDecimation;

	CComPtr<ID3D11Texture2D> sourceTexture;
	hr = desktopResource->QueryInterface(&sourceTexture);
	if (FAILED(hr))
	{
		if (d->isFrameAcquired)
		{
			SafeReleaseFrame(d->desktopDuplication);
			d->isFrameAcquired = false;
		}
		RETURN_IF_ERROR(hr, "Failed to get 2D texture from resource", -1);
	}

	// Ensure the processing pipeline matches the format of the delivered frame
	D3D11_TEXTURE2D_DESC sourceDesc{};
	sourceTexture->GetDesc(&sourceDesc);
	if (sourceDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && sourceDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		qCDebug(grabber_screen_capture) << "Skipping frame with unsupported format" << static_cast<int>(sourceDesc.Format);
		return -1;
	}
	if (sourceDesc.Format != d->pipelineFormat)
	{
		Debug(_log, "Frame format changed from %d to %d. Recreating processing pipeline.", static_cast<int>(d->pipelineFormat), static_cast<int>(sourceDesc.Format));
		if (!createPipelineResources(sourceDesc.Format))
		{
			return -1;
		}
	}

	const bool isFp16Frame = (sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
	if (isFp16Frame && d->sdrWhiteLevelTimer.hasExpired(SDR_WHITE_LEVEL_REFRESH_INTERVAL.count()))
	{
		const float sdrWhiteNits = querySdrWhiteLevelNits(d->gdiDeviceName, d->sdrWhiteNits);
		if (std::abs(sdrWhiteNits - d->sdrWhiteNits) > 0.5F)
		{
			Info(_log, "SDR white level changed from %.0f to %.0f nits", d->sdrWhiteNits, sdrWhiteNits);
			d->sdrWhiteNits = sdrWhiteNits;
		}
		d->sdrWhiteLevelTimer.restart();
	}

	// Use the D2D pipeline to handle any transformation or format conversion.
	auto createSourceBitmap = [this, isFp16Frame](ID3D11Texture2D* texture, CComPtr<ID2D1Bitmap1>& bitmap) -> HRESULT
	{
		bitmap.Release();
		CComPtr<IDXGISurface> surface;
		HRESULT hrSurface = texture->QueryInterface(&surface);
		if (FAILED(hrSurface))
		{
			return hrSurface;
		}
		if (isFp16Frame)
		{
			const D2D1_BITMAP_PROPERTIES1 sourceProperties = D2D1::BitmapProperties1(
				D2D1_BITMAP_OPTIONS_NONE,
				D2D1::PixelFormat(DXGI_FORMAT_R16G16B16A16_FLOAT, D2D1_ALPHA_MODE_PREMULTIPLIED));
			return d->d2dContext->CreateBitmapFromDxgiSurface(surface, &sourceProperties, &bitmap);
		}
		return d->d2dContext->CreateBitmapFromDxgiSurface(surface, nullptr, &bitmap);
	};

	// Copies the duplicated frame into an own texture, which D2D can always read
	auto copySourceTexture = [this, &sourceDesc, &sourceTexture]() -> HRESULT
	{
		D3D11_TEXTURE2D_DESC copyDesc{};
		if (d->sourceCopyTexture)
		{
			d->sourceCopyTexture->GetDesc(&copyDesc);
		}
		if (!d->sourceCopyTexture || copyDesc.Width != sourceDesc.Width || copyDesc.Height != sourceDesc.Height || copyDesc.Format != sourceDesc.Format)
		{
			copyDesc = {};
			copyDesc.Width = sourceDesc.Width;
			copyDesc.Height = sourceDesc.Height;
			copyDesc.Format = sourceDesc.Format;
			copyDesc.MipLevels = 1;
			copyDesc.ArraySize = 1;
			copyDesc.SampleDesc.Count = 1;
			copyDesc.Usage = D3D11_USAGE_DEFAULT;
			copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			d->sourceCopyTexture.Release();
			HRESULT hrCreate = d->device->CreateTexture2D(&copyDesc, nullptr, &d->sourceCopyTexture);
			if (FAILED(hrCreate))
			{
				return hrCreate;
			}
		}
		d->deviceContext->CopyResource(d->sourceCopyTexture, sourceTexture);
		return S_OK;
	};

	CComPtr<ID2D1Bitmap1> sourceBitmap;
	if (!d->useSourceCopy)
	{
		hr = createSourceBitmap(sourceTexture, sourceBitmap);
		if (FAILED(hr))
		{
			Debug(_log, "D2D cannot read the duplicated frame directly (0x%x). Using an intermediate copy.", hr);
			d->useSourceCopy = true;
		}
	}
	if (d->useSourceCopy)
	{
		hr = copySourceTexture();
		if (SUCCEEDED(hr))
		{
			hr = createSourceBitmap(d->sourceCopyTexture, sourceBitmap);
		}
	}
	if (FAILED(hr) || sourceBitmap == nullptr)
	{
		Error(_log, "Failed to create source bitmap (format %d): 0x%x", static_cast<int>(sourceDesc.Format), static_cast<unsigned>(hr));
		return -1;
	}

	// Draw to our target (B8G8R8A8 or FP16), performing all transforms (using pre-calculated members)
	d->d2dContext->SetTarget(d->destBitmap);
	d->d2dContext->BeginDraw();
	d->d2dContext->SetTransform(d->orientationTransform);
	if (isFp16Frame)
	{
		// Overwrite the target, independent of the alpha values delivered
		d->d2dContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
	}
	d->d2dContext->DrawBitmap(sourceBitmap, d->destRect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, d->sourceRect);
	hr = d->d2dContext->EndDraw();
	d->d2dContext->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
	d->d2dContext->SetTarget(nullptr);

	// Copy the D2D result to the staging texture
	d->deviceContext->CopyResource(d->intermediateTexture, d->d2dConvertedTexture);
	d->deviceContext->Flush();

	if (FAILED(hr))
	{
		setInError(QString("D2D DrawBitmap failed: 0x%1").arg(hr));
		return -1;
	}

	// Map and copy to user image
	D3D11_MAPPED_SUBRESOURCE mapped;
	hr = d->deviceContext->Map(d->intermediateTexture, 0, D3D11_MAP_READ, 0, &mapped);
	RETURN_IF_ERROR(hr, "Failed to map final texture", -1);

	ColorRgb* destPtr = image.memptr();
	const auto* srcPtr = static_cast<const uint8_t*>(mapped.pData);

	if (isFp16Frame)
	{
		// CPU conversion from FP16 scRGB (linear, 1.0 = 80 nits) to 8-bit sRGB.
		// SDR content is mapped to full range using the Windows SDR white level,
		// brighter HDR highlights are scaled down preserving the hue.
		const float scale = SCRGB_REFERENCE_NITS / std::max(d->sdrWhiteNits, 1.0F);
		const uint8_t* lut = d->linearToSrgbLut.data();
		// Half -> linear value relative to SDR white. Negative (out of sRGB gamut), NaN and Inf values become 0.
		auto toLinear = [scale](DirectX::PackedVector::HALF value) -> float
		{
			const float linear = DirectX::PackedVector::XMConvertHalfToFloat(value) * scale;
			return (std::isfinite(linear) && linear > 0.0F) ? linear : 0.0F;
		};
		// Linear [0..1] -> 8-bit sRGB
		auto toSrgb = [lut](float linear) -> uint8_t
		{
			return lut[static_cast<int>(linear * SRGB_LUT_SIZE + 0.5F)];
		};

		for (int y = 0; y < _height; ++y)
		{
			const auto* srcRowPtr = static_cast<const DirectX::PackedVector::HALF*>(static_cast<const void*>(srcPtr + y * mapped.RowPitch));
			ColorRgb* destRowPtr = destPtr + y * _width;

			for (int x = 0; x < _width; ++x)
			{
				float red = toLinear(srcRowPtr[0]);
				float green = toLinear(srcRowPtr[1]);
				float blue = toLinear(srcRowPtr[2]);

				const float maxChannel = std::max({ red, green, blue });
				if (maxChannel > 1.0F)
				{
					red /= maxChannel;
					green /= maxChannel;
					blue /= maxChannel;
				}

				destRowPtr->red = toSrgb(std::min(red, 1.0F));
				destRowPtr->green = toSrgb(std::min(green, 1.0F));
				destRowPtr->blue = toSrgb(std::min(blue, 1.0F));

				// Move to the next pixel (RGBA)
				srcRowPtr += 4;
				destRowPtr++;
			}
		}

		d->deviceContext->Unmap(d->intermediateTexture, 0);
		return 0;
	}

	// CPU pixel copy from BGRA to output RGB image
	for (int y = 0; y < _height; ++y)
	{
		// Set pointers to the start of the current row
		const auto* srcRowPtr = static_cast<const uint32_t*>(static_cast<const void*>(srcPtr + y * mapped.RowPitch));
		ColorRgb* destRowPtr = destPtr + y * _width;

		for (int x = 0; x < _width; ++x)
		{
			const auto* bgra = static_cast<const std::byte*>(static_cast<const void*>(srcRowPtr));
			destRowPtr->red = static_cast<uint8_t>(bgra[2]);
			destRowPtr->green = static_cast<uint8_t>(bgra[1]);
			destRowPtr->blue = static_cast<uint8_t>(bgra[0]);

			// Move to the next pixel
			srcRowPtr++;
			destRowPtr++;
		}
	}

	d->deviceContext->Unmap(d->intermediateTexture, 0);

	// The frame is intentionally held until the next grabFrame call for performance.
	return 0;
}

void DDAGrabber::computeCropBox(int sourceWidth, int sourceHeight, D3D11_BOX& box) const
{
	switch (d->desktopRotation) {
	case DXGI_MODE_ROTATION_ROTATE90:
		box.left = _cropTop;
		box.right = sourceWidth - _cropBottom;
		box.top = _cropRight;
		box.bottom = sourceHeight - _cropLeft;
		break;
	case DXGI_MODE_ROTATION_ROTATE270:
		box.left = _cropBottom;
		box.right = sourceWidth - _cropTop;
		box.top = _cropLeft;
		box.bottom = sourceHeight - _cropRight;
		break;
	case DXGI_MODE_ROTATION_ROTATE180:
		box.left = _cropRight;
		box.right = sourceWidth - _cropLeft;
		box.top = _cropBottom;
		box.bottom = sourceHeight - _cropTop;
		break;
	case DXGI_MODE_ROTATION_IDENTITY:
	default:
		box.left = _cropLeft;
		box.right = sourceWidth - _cropRight;
		box.top = _cropTop;
		box.bottom = sourceHeight - _cropBottom;
		break;
	}

	box.front = 0;
	box.back = 1;
}

bool DDAGrabber::setDisplayIndex(int index)
{
	qCDebug(grabber_screen_flow) << "Setting display index to" << index << ", current is" << d->display;
	bool rc = true;
	if (d->display != index)
	{
		d->display = index;
		rc = resetDeviceAndCapture();
	}
	return rc;
}
QJsonObject DDAGrabber::discover(const QJsonObject& params)
{
	QJsonObject inputsDiscovered;
	if (isAvailable(false) && setupDisplay())
	{
		HRESULT hr = S_OK;

		setFpsSupported(QJsonArray{1, 5, 10, 15, 20, 25, 30, 40, 50, 60, 120, 144});
		// Enumerate through the outputs.
		QJsonArray videoInputs;
		for (int i = 0;; ++i)
		{
			CComPtr<IDXGIOutput> output;
			hr = d->dxgiAdapter->EnumOutputs(i, &output);
			if (!output || !SUCCEEDED(hr))
			{
				break;
			}

			// Get the output description.
			DXGI_OUTPUT_DESC desc;
			hr = output->GetDesc(&desc);
			if (FAILED(hr))
			{
				Error(_log, "Failed to get output description");
				continue;
			}

			// Add it to the JSON.
			const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
			const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

			qCDebug(grabber_screen_properties) << "Found video input" << i << "with name" << QString::fromWCharArray(desc.DeviceName)
				<< "and size" << width << "x" << height;

			videoInputs.append(QJsonObject{
				{"inputIdx", i},
				{"name", QString::fromWCharArray(desc.DeviceName)},
				{"formats",
				 QJsonArray{
					 QJsonObject{
						 {"resolutions",
						  QJsonArray{
							  QJsonObject{
								  {"width", width},
								  {"height", height},
								  {"fps", getFpsSupported()},
							  },
						  }},
					 },
				 }},
				});
		}

		inputsDiscovered["video_inputs"] = videoInputs;
		if (!videoInputs.isEmpty())
		{
			inputsDiscovered["device"] = "dda";
			inputsDiscovered["device_name"] = "DXGI DDA";
			inputsDiscovered["type"] = "screen";
			inputsDiscovered["default"] = QJsonObject{
				{"video_input",
				 QJsonObject{
					 {"inputIdx", 0},
					 {"resolution",
					  QJsonObject{
						  {"fps", 60},
					  }},
				 }},
			};
		}
	}

	if (inputsDiscovered.isEmpty())
	{
		qCDebug(grabber_screen_properties) << "No displays found to capture from!";
	}
	return inputsDiscovered;
}
