#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace dwm_overlay {

// Decides, entirely on the GPU, whether the overlay region was recomposed by
// DWM since the previous present, and restores the freshest clean backdrop:
//
//   current region == previous final output  ->  region still holds our UI
//                                               (no recomposition); keep the
//                                               saved backdrop.
//   current region != previous final output  ->  DWM recomposed the region;
//                                               the fresh content becomes the
//                                               new backdrop.
//
// The restored backdrop is copied onto the back buffer with a plain
// CopySubresourceRegion and the UI is then alpha-blended by ImGui on top, so
// the only shader work runs on overlay-owned textures. This stays stable
// whether or not DWM's partial recomposition lands on a given frame
// (wallpaper/desktop repaints are irregular), while windows passing over the
// overlay leave no ghosting.
class BackdropCompositor final {
public:
	// (Re)creates the region-sized resources; returns false when the backing
	// device cannot support the pipeline and the caller should draw directly.
	bool EnsureSize(
		ID3D11Device* device,
		const D3D11_TEXTURE2D_DESC& backBufferDescription) noexcept;
	void Shutdown() noexcept;

	// Copies the overlay region of the back buffer into the comparison source.
	void CaptureRegion(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* backBuffer,
		const RECT& region) noexcept;

	// Runs the discriminate pass on overlay-owned textures and copies the
	// restored clean backdrop onto the back buffer region. Returns false when
	// the shaders are unavailable; the caller then draws without a restore.
	bool RestoreBackdrop(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* backBuffer,
		const RECT& region) noexcept;

	// Saves the final region content (UI included) for the next comparison.
	void SaveOutput(
		ID3D11DeviceContext* context,
		ID3D11Texture2D* backBuffer,
		const RECT& region) noexcept;

private:
	struct Constants {
		float threshold;
		float padding[3];
	};

	bool EnsureShaders(ID3D11Device* device) noexcept;
	void ReleaseSizeDependent() noexcept;

	Microsoft::WRL::ComPtr<ID3D11Texture2D> regionTexture_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> regionView_;
	Microsoft::WRL::ComPtr<ID3D11Texture2D> lastOutputTexture_;
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> lastOutputView_;
	// Ping-pong pair: the discriminate pass reads one and writes the other.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> backdropTextures_[2];
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backdropTargets_[2];
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> backdropViews_[2];
	UINT backdropRead_ = 0;

	Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
	Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
	Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
	bool shadersTried_ = false;

	UINT width_ = 0;
	UINT height_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

} // namespace dwm_overlay
