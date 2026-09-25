#include "BackdropCompositor.h"

#include "../Common/Log.h"

#include <d3dcompiler.h>

#include <cstring>
#include <vector>

namespace dwm_overlay {
namespace {

// Covers the render-target viewport with one triangle; no vertex buffer is
// bound, the position is generated from SV_VertexID.
static const char kVertexShader[] = R"(
struct VsOut {
	float4 position : SV_POSITION;
};

VsOut main(uint vertexId : SV_VertexID) {
	VsOut output;
	float2 uv = float2((vertexId << 1) & 2u, vertexId & 2u);
	output.position = float4(uv * float2(4.0f, -4.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
	return output;
}
)";

// Region content equal to the previous final output means DWM has not
// recomposed this frame: keep the saved backdrop. Anything else is fresh
// composition and becomes the new backdrop. SV_Position is already in render
// target space, so no region offset is applied to the texel fetches.
static const char kPixelShader[] = R"(
Texture2D texRegion : register(t0);
Texture2D texLastOutput : register(t1);
Texture2D texBackdrop : register(t2);

cbuffer RegionConstants : register(b0) {
	float threshold;
	float3 padding;
};

float4 main(float4 position : SV_POSITION) : SV_Target0 {
	int2 pixel = int2(position.xy);
	float4 region = texRegion.Load(int3(pixel, 0));
	float4 lastOutput = texLastOutput.Load(int3(pixel, 0));
	float4 backdrop = region;
	if (all(abs(region - lastOutput) <= threshold)) {
		backdrop = texBackdrop.Load(int3(pixel, 0));
	}
	return backdrop;
}
)";

bool CreateRegionTexture(
	ID3D11Device* device,
	const D3D11_TEXTURE2D_DESC& backBufferDescription,
	UINT bindFlags,
	bool zeroInitialize,
	Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture) noexcept {
	D3D11_TEXTURE2D_DESC description = backBufferDescription;
	description.MipLevels = 1;
	description.ArraySize = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = bindFlags;
	description.SampleDesc = { 1, 0 };
	description.MiscFlags = 0;

	D3D11_SUBRESOURCE_DATA initial = {};
	std::vector<BYTE> zeroData;
	if (zeroInitialize) {
		// pSysMem must contain the WHOLE texture; a row-sized buffer makes
		// D3D read past the allocation.
		const size_t pitch = static_cast<size_t>(description.Width) * 4;
		zeroData.resize(pitch * description.Height, 0);
		initial.pSysMem = zeroData.data();
		initial.SysMemPitch = static_cast<UINT>(pitch);
	}
	return SUCCEEDED(device->CreateTexture2D(
		&description, zeroInitialize ? &initial : nullptr, &texture));
}

bool CreateShaderResourceView(
	ID3D11Device* device,
	ID3D11Texture2D* texture,
	const DXGI_FORMAT format,
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view) noexcept {
	D3D11_SHADER_RESOURCE_VIEW_DESC description = {};
	description.Format = format;
	description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	description.Texture2D.MostDetailedMip = 0;
	description.Texture2D.MipLevels = 1;
	return SUCCEEDED(device->CreateShaderResourceView(
		texture, &description, &view));
}

bool CreateRenderTargetView(
	ID3D11Device* device,
	ID3D11Texture2D* texture,
	const DXGI_FORMAT format,
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& view) noexcept {
	D3D11_RENDER_TARGET_VIEW_DESC description = {};
	description.Format = format;
	description.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	return SUCCEEDED(device->CreateRenderTargetView(
		texture, &description, &view));
}

} // namespace

bool BackdropCompositor::EnsureSize(
	ID3D11Device* device,
	const D3D11_TEXTURE2D_DESC& backBufferDescription) noexcept {
	if (backBufferDescription.SampleDesc.Count != 1 ||
		backBufferDescription.SampleDesc.Quality != 0)
		return false; // MSAA back buffers would need a resolve; not supported.
	if (backBufferDescription.Width == width_ &&
		backBufferDescription.Height == height_ &&
		backBufferDescription.Format == format_)
		return pixelShader_ != nullptr;

	if (!EnsureShaders(device))
		return false;

	ReleaseSizeDependent();
	width_ = backBufferDescription.Width;
	height_ = backBufferDescription.Height;
	format_ = backBufferDescription.Format;

	// Comparison sources hold shader-resource data only. A zeroed
	// lastOutputTexture simply fails the first comparison and adopts the
	// region content as the initial backdrop, so no clear pass is required.
	if (!CreateRegionTexture(device, backBufferDescription,
		D3D11_BIND_SHADER_RESOURCE, false, regionTexture_) ||
		!CreateShaderResourceView(device, regionTexture_.Get(),
			format_, regionView_))
	{
		ReleaseSizeDependent();
		return false;
	}
	if (!CreateRegionTexture(device, backBufferDescription,
		D3D11_BIND_SHADER_RESOURCE, true, lastOutputTexture_) ||
		!CreateShaderResourceView(device, lastOutputTexture_.Get(),
			format_, lastOutputView_))
	{
		ReleaseSizeDependent();
		return false;
	}

	for (int index = 0; index < 2; ++index) {
		if (!CreateRegionTexture(device, backBufferDescription,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, false,
			backdropTextures_[index]) ||
			!CreateShaderResourceView(device, backdropTextures_[index].Get(),
				format_, backdropViews_[index]) ||
			!CreateRenderTargetView(device, backdropTextures_[index].Get(),
				format_, backdropTargets_[index]))
		{
			ReleaseSizeDependent();
			return false;
		}
	}
	DWM_LOG("Backdrop compositor resources ready");
	return true;
}

void BackdropCompositor::Shutdown() noexcept {
	ReleaseSizeDependent();
	vertexShader_.Reset();
	pixelShader_.Reset();
	constantBuffer_.Reset();
	shadersTried_ = false;
}

void BackdropCompositor::ReleaseSizeDependent() noexcept {
	regionView_.Reset();
	regionTexture_.Reset();
	lastOutputView_.Reset();
	lastOutputTexture_.Reset();
	for (int index = 0; index < 2; ++index) {
		backdropViews_[index].Reset();
		backdropTargets_[index].Reset();
		backdropTextures_[index].Reset();
	}
	backdropRead_ = 0;
	width_ = 0;
	height_ = 0;
	format_ = DXGI_FORMAT_UNKNOWN;
}

bool BackdropCompositor::EnsureShaders(ID3D11Device* device) noexcept {
	if (pixelShader_)
		return true;
	if (shadersTried_)
		return false;
	shadersTried_ = true;

	Microsoft::WRL::ComPtr<ID3DBlob> code;
	if (FAILED(D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, nullptr,
		nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr))) {
		DWM_LOG_ONCE("Backdrop compositor vertex shader compilation failed");
		return false;
	}
	if (FAILED(device->CreateVertexShader(code->GetBufferPointer(),
		code->GetBufferSize(), nullptr, &vertexShader_))) {
		DWM_LOG_ONCE("Backdrop compositor vertex shader creation failed");
		return false;
	}
	if (FAILED(D3DCompile(kPixelShader, sizeof(kPixelShader) - 1, nullptr,
		nullptr, nullptr, "main", "ps_5_0", 0, 0, &code, nullptr))) {
		DWM_LOG_ONCE("Backdrop compositor pixel shader compilation failed");
		return false;
	}
	if (FAILED(device->CreatePixelShader(code->GetBufferPointer(),
		code->GetBufferSize(), nullptr, &pixelShader_))) {
		DWM_LOG_ONCE("Backdrop compositor pixel shader creation failed");
		return false;
	}

	D3D11_BUFFER_DESC constantDescription = {};
	constantDescription.ByteWidth = sizeof(Constants);
	constantDescription.Usage = D3D11_USAGE_DYNAMIC;
	constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&constantDescription, nullptr,
		&constantBuffer_))) {
		DWM_LOG_ONCE("Backdrop compositor constant buffer creation failed");
		constantBuffer_.Reset();
		return false;
	}
	DWM_LOG("Backdrop compositor shaders ready");
	return true;
}

void BackdropCompositor::CaptureRegion(
	ID3D11DeviceContext* context,
	ID3D11Texture2D* backBuffer,
	const RECT& region) noexcept {
	if (!regionTexture_)
		return;
	D3D11_BOX box = {};
	box.left = static_cast<UINT>(region.left);
	box.top = static_cast<UINT>(region.top);
	box.right = static_cast<UINT>(region.right);
	box.bottom = static_cast<UINT>(region.bottom);
	box.front = 0;
	box.back = 1;
	context->CopySubresourceRegion(regionTexture_.Get(), 0,
		static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0,
		backBuffer, 0, &box);
}

bool BackdropCompositor::RestoreBackdrop(
	ID3D11DeviceContext* context,
	ID3D11Texture2D* backBuffer,
	const RECT& region) noexcept {
	if (!pixelShader_)
		return false;

	Constants constants = {};
	constants.threshold = 0.5f / 255.0f;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(constantBuffer_.Get(), 0,
		D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(constantBuffer_.Get(), 0);

	const UINT backdropWrite = backdropRead_ ^ 1;
	context->OMSetRenderTargets(1, backdropTargets_[backdropWrite].GetAddressOf(), nullptr);
	const D3D11_VIEWPORT viewport = {
		static_cast<FLOAT>(region.left),
		static_cast<FLOAT>(region.top),
		static_cast<FLOAT>(region.right - region.left),
		static_cast<FLOAT>(region.bottom - region.top),
		0.0f, 1.0f
	};
	context->RSSetViewports(1, &viewport);
	context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
	context->OMSetDepthStencilState(nullptr, 0);
	context->RSSetState(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->IASetInputLayout(nullptr);
	context->VSSetShader(vertexShader_.Get(), nullptr, 0);
	context->PSSetShader(pixelShader_.Get(), nullptr, 0);
	context->PSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
	ID3D11ShaderResourceView* views[3] = {
		regionView_.Get(), lastOutputView_.Get(), backdropViews_[backdropRead_].Get()
	};
	context->PSSetShaderResources(0, 3, views);
	context->Draw(3, 0);

	// Unbind before these textures become copy destinations again.
	ID3D11ShaderResourceView* nullViews[3] = {};
	context->PSSetShaderResources(0, 3, nullViews);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	// The discriminate pass wrote the restored backdrop into an overlay-owned
	// texture; place it onto the back buffer with a plain copy, exactly like
	// the classic restore path.
	D3D11_BOX box = {};
	box.left = static_cast<UINT>(region.left);
	box.top = static_cast<UINT>(region.top);
	box.right = static_cast<UINT>(region.right);
	box.bottom = static_cast<UINT>(region.bottom);
	box.front = 0;
	box.back = 1;
	context->CopySubresourceRegion(backBuffer, 0,
		static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0,
		backdropTextures_[backdropWrite].Get(), 0, &box);

	backdropRead_ = backdropWrite;
	return true;
}

void BackdropCompositor::SaveOutput(
	ID3D11DeviceContext* context,
	ID3D11Texture2D* backBuffer,
	const RECT& region) noexcept {
	if (!lastOutputTexture_)
		return;
	D3D11_BOX box = {};
	box.left = static_cast<UINT>(region.left);
	box.top = static_cast<UINT>(region.top);
	box.right = static_cast<UINT>(region.right);
	box.bottom = static_cast<UINT>(region.bottom);
	box.front = 0;
	box.back = 1;
	context->CopySubresourceRegion(lastOutputTexture_.Get(), 0,
		static_cast<UINT>(region.left), static_cast<UINT>(region.top), 0,
		backBuffer, 0, &box);
}

} // namespace dwm_overlay
