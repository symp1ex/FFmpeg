/*
 * Copyright (C) 2025 MulticorewWare, Inc.
 *
 * Authors: Dash Santosh <dash.sathanatayanan@multicorewareinc.com>
 *          Sachin <sachin.prakash@multicorewareinc.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "compat/w32dlfcn.h"

#include <initguid.h>
#include <d3dcompiler.h>
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_d3d11va.h"

#include "filters.h"
#include "scale_eval.h"
#include "video.h"

typedef struct ScaleD3D11Context {
    const AVClass *classCtx;
    char *w_expr;
    char *h_expr;
    enum AVPixelFormat format;
    int color_bits;

    ///< D3D11 objects
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VideoDevice *videoDevice;
    ID3D11VideoProcessor *processor;
    ID3D11VideoProcessorEnumerator *enumerator;
    ID3D11VideoProcessorOutputView *outputView;
    ID3D11VideoProcessorInputView *inputView;
    ID3D11VertexShader *quant_vs;
    ID3D11PixelShader *quant_y_ps;
    ID3D11PixelShader *quant_uv_ps;
    ID3D11SamplerState *quant_sampler;
    ID3D11Buffer *quant_const_buffer;
    ID3D11Texture2D *quant_input_texture;
    ID3D11ShaderResourceView *quant_input_view;
    void *d3dcompiler;

    ///< Buffer references
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx_out;

    ///< Dimensions and formats
    int width, height;
    int inputWidth, inputHeight;
    DXGI_FORMAT input_format;
    DXGI_FORMAT output_format;
    int quant_input_width, quant_input_height;
    DXGI_FORMAT quant_input_format;
} ScaleD3D11Context;

typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID pSrcData,
                                       SIZE_T SrcDataSize,
                                       LPCSTR pSourceName,
                                       const D3D_SHADER_MACRO *pDefines,
                                       ID3DInclude *pInclude,
                                       LPCSTR pEntrypoint,
                                       LPCSTR pTarget,
                                       UINT Flags1,
                                       UINT Flags2,
                                       ID3DBlob **ppCode,
                                       ID3DBlob **ppErrorMsgs);

typedef struct QuantConstBuffer {
    float src_size[2];
    float dst_size[2];
    uint32_t color_bits;
    uint32_t padding[3];
} QuantConstBuffer;

static const char scale_d3d11_quant_shader[] =
    "cbuffer Params : register(b0) {"
    "  float2 srcSize;"
    "  float2 dstSize;"
    "  uint colorBits;"
    "  uint3 padding;"
    "};"
    "Texture2D srcTex : register(t0);"
    "SamplerState srcSampler : register(s0);"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
    "VSOut VSMain(uint id : SV_VertexID) {"
    "  float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };"
    "  float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };"
    "  VSOut o;"
    "  o.pos = float4(pos[id], 0.0, 1.0);"
    "  o.uv = uv[id];"
    "  return o;"
    "}"
    "float3 Quantize(float3 c) {"
    "  if (colorBits == 16) {"
    "    c.r = round(saturate(c.r) * 31.0) / 31.0;"
    "    c.g = round(saturate(c.g) * 63.0) / 63.0;"
    "    c.b = round(saturate(c.b) * 31.0) / 31.0;"
    "  } else if (colorBits == 8) {"
    "    c.r = round(saturate(c.r) * 7.0) / 7.0;"
    "    c.g = round(saturate(c.g) * 7.0) / 7.0;"
    "    c.b = round(saturate(c.b) * 3.0) / 3.0;"
    "  }"
    "  return c;"
    "}"
    "float3 RGBToYUV(float3 c) {"
    "  float y = 16.0 / 255.0 + dot(c, float3(0.182586, 0.614231, 0.062007));"
    "  float u = 128.0 / 255.0 + dot(c, float3(-0.100644, -0.338572, 0.439216));"
    "  float v = 128.0 / 255.0 + dot(c, float3(0.439216, -0.398942, -0.040274));"
    "  return saturate(float3(y, u, v));"
    "}"
    "float4 PSY(VSOut i) : SV_TARGET {"
    "  float3 rgb = Quantize(srcTex.SampleLevel(srcSampler, i.uv, 0).rgb);"
    "  return float4(RGBToYUV(rgb).x, 0.0, 0.0, 1.0);"
    "}"
    "float4 PSUV(VSOut i) : SV_TARGET {"
    "  float2 texel = 1.0 / srcSize;"
    "  float2 base = i.uv;"
    "  float3 c0 = Quantize(srcTex.SampleLevel(srcSampler, base + texel * float2(-0.5, -0.5), 0).rgb);"
    "  float3 c1 = Quantize(srcTex.SampleLevel(srcSampler, base + texel * float2( 0.5, -0.5), 0).rgb);"
    "  float3 c2 = Quantize(srcTex.SampleLevel(srcSampler, base + texel * float2(-0.5,  0.5), 0).rgb);"
    "  float3 c3 = Quantize(srcTex.SampleLevel(srcSampler, base + texel * float2( 0.5,  0.5), 0).rgb);"
    "  float3 yuv = RGBToYUV((c0 + c1 + c2 + c3) * 0.25);"
    "  return float4(yuv.y, yuv.z, 0.0, 1.0);"
    "}";

static av_cold int scale_d3d11_init(AVFilterContext *ctx) {
    ScaleD3D11Context *s = ctx->priv;

    if (s->color_bits && s->color_bits != 16 && s->color_bits != 8) {
        av_log(ctx, AV_LOG_ERROR, "color_bits must be 0, 16, or 8\n");
        return AVERROR(EINVAL);
    }

    ///< all real work is done in config_props and filter_frame
    return 0;
}

static void release_d3d11_resources(ScaleD3D11Context *s) {
    if (s->outputView) {
        s->outputView->lpVtbl->Release(s->outputView);
        s->outputView = NULL;
    }

    if (s->processor) {
        s->processor->lpVtbl->Release(s->processor);
        s->processor = NULL;
    }

    if (s->enumerator) {
        s->enumerator->lpVtbl->Release(s->enumerator);
        s->enumerator = NULL;
    }

    if (s->videoDevice) {
        s->videoDevice->lpVtbl->Release(s->videoDevice);
        s->videoDevice = NULL;
    }

    if (s->quant_input_view) {
        s->quant_input_view->lpVtbl->Release(s->quant_input_view);
        s->quant_input_view = NULL;
    }

    if (s->quant_input_texture) {
        s->quant_input_texture->lpVtbl->Release(s->quant_input_texture);
        s->quant_input_texture = NULL;
    }

    if (s->quant_const_buffer) {
        s->quant_const_buffer->lpVtbl->Release(s->quant_const_buffer);
        s->quant_const_buffer = NULL;
    }

    if (s->quant_sampler) {
        s->quant_sampler->lpVtbl->Release(s->quant_sampler);
        s->quant_sampler = NULL;
    }

    if (s->quant_uv_ps) {
        s->quant_uv_ps->lpVtbl->Release(s->quant_uv_ps);
        s->quant_uv_ps = NULL;
    }

    if (s->quant_y_ps) {
        s->quant_y_ps->lpVtbl->Release(s->quant_y_ps);
        s->quant_y_ps = NULL;
    }

    if (s->quant_vs) {
        s->quant_vs->lpVtbl->Release(s->quant_vs);
        s->quant_vs = NULL;
    }

    if (s->d3dcompiler) {
        dlclose(s->d3dcompiler);
        s->d3dcompiler = NULL;
    }
}

static int scale_d3d11_compile_shader(ScaleD3D11Context *s, AVFilterContext *ctx,
                                      const char *entry, const char *target,
                                      ID3DBlob **blob)
{
    D3DCompileFn d3d_compile;
    ID3DBlob *errors = NULL;
    HRESULT hr;

    if (!s->d3dcompiler) {
        s->d3dcompiler = dlopen("d3dcompiler_47.dll", 0);
        if (!s->d3dcompiler)
            s->d3dcompiler = dlopen("d3dcompiler_43.dll", 0);
        if (!s->d3dcompiler) {
            av_log(ctx, AV_LOG_ERROR, "Failed to load d3dcompiler DLL for color quantization\n");
            return AVERROR_EXTERNAL;
        }
    }

    d3d_compile = (D3DCompileFn)dlsym(s->d3dcompiler, "D3DCompile");
    if (!d3d_compile) {
        av_log(ctx, AV_LOG_ERROR, "Failed to load D3DCompile for color quantization\n");
        return AVERROR_EXTERNAL;
    }

    hr = d3d_compile(scale_d3d11_quant_shader, sizeof(scale_d3d11_quant_shader) - 1,
                     "scale_d3d11_quant_shader", NULL, NULL, entry, target,
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            av_log(ctx, AV_LOG_ERROR, "Failed to compile %s: %.*s\n", entry,
                   (int)errors->lpVtbl->GetBufferSize(errors),
                   (char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        } else {
            av_log(ctx, AV_LOG_ERROR, "Failed to compile %s: HRESULT 0x%lX\n", entry, hr);
        }
        return AVERROR_EXTERNAL;
    }

    if (errors)
        errors->lpVtbl->Release(errors);
    return 0;
}

static int scale_d3d11_init_quant_resources(ScaleD3D11Context *s, AVFilterContext *ctx)
{
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *y_blob = NULL;
    ID3DBlob *uv_blob = NULL;
    D3D11_SAMPLER_DESC sampler_desc = { 0 };
    D3D11_BUFFER_DESC buffer_desc = { 0 };
    HRESULT hr;
    int ret;

    if (s->quant_vs && s->quant_y_ps && s->quant_uv_ps &&
        s->quant_sampler && s->quant_const_buffer)
        return 0;

    ret = scale_d3d11_compile_shader(s, ctx, "VSMain", "vs_4_0", &vs_blob);
    if (ret < 0)
        goto fail;
    ret = scale_d3d11_compile_shader(s, ctx, "PSY", "ps_4_0", &y_blob);
    if (ret < 0)
        goto fail;
    ret = scale_d3d11_compile_shader(s, ctx, "PSUV", "ps_4_0", &uv_blob);
    if (ret < 0)
        goto fail;

    hr = s->device->lpVtbl->CreateVertexShader(s->device,
                                               vs_blob->lpVtbl->GetBufferPointer(vs_blob),
                                               vs_blob->lpVtbl->GetBufferSize(vs_blob),
                                               NULL, &s->quant_vs);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreateVertexShader for color quantization failed: 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    hr = s->device->lpVtbl->CreatePixelShader(s->device,
                                              y_blob->lpVtbl->GetBufferPointer(y_blob),
                                              y_blob->lpVtbl->GetBufferSize(y_blob),
                                              NULL, &s->quant_y_ps);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreatePixelShader Y for color quantization failed: 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    hr = s->device->lpVtbl->CreatePixelShader(s->device,
                                              uv_blob->lpVtbl->GetBufferPointer(uv_blob),
                                              uv_blob->lpVtbl->GetBufferSize(uv_blob),
                                              NULL, &s->quant_uv_ps);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreatePixelShader UV for color quantization failed: 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = s->device->lpVtbl->CreateSamplerState(s->device, &sampler_desc, &s->quant_sampler);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreateSamplerState for color quantization failed: 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    buffer_desc.ByteWidth = sizeof(QuantConstBuffer);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = s->device->lpVtbl->CreateBuffer(s->device, &buffer_desc, NULL, &s->quant_const_buffer);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreateBuffer for color quantization failed: 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ret = 0;

fail:
    if (vs_blob)
        vs_blob->lpVtbl->Release(vs_blob);
    if (y_blob)
        y_blob->lpVtbl->Release(y_blob);
    if (uv_blob)
        uv_blob->lpVtbl->Release(uv_blob);
    return ret;
}

static int scale_d3d11_prepare_quant_input(ScaleD3D11Context *s, AVFilterContext *ctx,
                                           ID3D11Texture2D *input_texture,
                                           const D3D11_TEXTURE2D_DESC *input_desc,
                                           int sub_idx)
{
    D3D11_TEXTURE2D_DESC copy_desc;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = { 0 };
    HRESULT hr;

    if (!s->quant_input_texture ||
        s->quant_input_width != (int)input_desc->Width ||
        s->quant_input_height != (int)input_desc->Height ||
        s->quant_input_format != input_desc->Format) {
        if (s->quant_input_view) {
            s->quant_input_view->lpVtbl->Release(s->quant_input_view);
            s->quant_input_view = NULL;
        }
        if (s->quant_input_texture) {
            s->quant_input_texture->lpVtbl->Release(s->quant_input_texture);
            s->quant_input_texture = NULL;
        }

        copy_desc = *input_desc;
        copy_desc.ArraySize = 1;
        copy_desc.MipLevels = 1;
        copy_desc.Usage = D3D11_USAGE_DEFAULT;
        copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        copy_desc.CPUAccessFlags = 0;
        copy_desc.MiscFlags = 0;

        hr = s->device->lpVtbl->CreateTexture2D(s->device, &copy_desc, NULL, &s->quant_input_texture);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "CreateTexture2D for color quantization input failed: 0x%lX\n", hr);
            return AVERROR_EXTERNAL;
        }

        srv_desc.Format = copy_desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = 1;
        hr = s->device->lpVtbl->CreateShaderResourceView(s->device,
                                                         (ID3D11Resource *)s->quant_input_texture,
                                                         &srv_desc,
                                                         &s->quant_input_view);
        if (FAILED(hr)) {
            av_log(ctx, AV_LOG_ERROR, "CreateShaderResourceView for color quantization failed: 0x%lX\n", hr);
            return AVERROR_EXTERNAL;
        }

        s->quant_input_width = input_desc->Width;
        s->quant_input_height = input_desc->Height;
        s->quant_input_format = input_desc->Format;
    }

    s->context->lpVtbl->CopySubresourceRegion(s->context,
                                              (ID3D11Resource *)s->quant_input_texture,
                                              0, 0, 0, 0,
                                              (ID3D11Resource *)input_texture,
                                              sub_idx, NULL);
    return 0;
}

static int scale_d3d11_quantize_frame(ScaleD3D11Context *s, AVFilterContext *ctx,
                                      AVFrame *in, AVFrame *out)
{
    ID3D11Texture2D *input_texture = (ID3D11Texture2D *)in->data[0];
    ID3D11Texture2D *output_texture = (ID3D11Texture2D *)out->data[0];
    D3D11_TEXTURE2D_DESC input_desc;
    D3D11_RENDER_TARGET_VIEW_DESC y_rtv_desc = { 0 };
    D3D11_RENDER_TARGET_VIEW_DESC uv_rtv_desc = { 0 };
    ID3D11RenderTargetView *y_rtv = NULL;
    ID3D11RenderTargetView *uv_rtv = NULL;
    ID3D11RenderTargetView *null_rtv = NULL;
    ID3D11ShaderResourceView *null_srv = NULL;
    D3D11_VIEWPORT viewport;
    QuantConstBuffer params;
    HRESULT hr;
    int ret;
    int sub_idx = (int)(intptr_t)in->data[1];

    if (s->format != AV_PIX_FMT_NV12) {
        av_log(ctx, AV_LOG_ERROR, "scale_d3d11 color_bits currently supports only NV12 output\n");
        return AVERROR(EINVAL);
    }

    input_texture->lpVtbl->GetDesc(input_texture, &input_desc);
    if (input_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        input_desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        av_log(ctx, AV_LOG_ERROR,
               "scale_d3d11 color_bits requires BGRA/RGBA 8-bit input, got DXGI format %d\n",
               input_desc.Format);
        return AVERROR(EINVAL);
    }

    ret = scale_d3d11_init_quant_resources(s, ctx);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 color quantization: color_bits=%d input=%ux%u output=%dx%d\n",
           s->color_bits, input_desc.Width, input_desc.Height, s->width, s->height);

    ret = scale_d3d11_prepare_quant_input(s, ctx, input_texture, &input_desc, sub_idx);
    if (ret < 0)
        return ret;
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 color quantization: input copy prepared\n");

    y_rtv_desc.Format = DXGI_FORMAT_R8_UNORM;
    y_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    y_rtv_desc.Texture2D.MipSlice = 0;
    hr = s->device->lpVtbl->CreateRenderTargetView(s->device,
                                                   (ID3D11Resource *)output_texture,
                                                   &y_rtv_desc,
                                                   &y_rtv);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreateRenderTargetView Y for color quantization failed: 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 color quantization: Y render target ready\n");

    uv_rtv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
    uv_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    uv_rtv_desc.Texture2D.MipSlice = 0;
    hr = s->device->lpVtbl->CreateRenderTargetView(s->device,
                                                   (ID3D11Resource *)output_texture,
                                                   &uv_rtv_desc,
                                                   &uv_rtv);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "CreateRenderTargetView UV for color quantization failed: 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 color quantization: UV render target ready\n");

    params = (QuantConstBuffer) {
        .src_size = { (float)input_desc.Width, (float)input_desc.Height },
        .dst_size = { (float)s->width, (float)s->height },
        .color_bits = s->color_bits,
    };
    s->context->lpVtbl->UpdateSubresource(s->context,
                                          (ID3D11Resource *)s->quant_const_buffer,
                                          0, NULL, &params, 0, 0);

    s->context->lpVtbl->IASetInputLayout(s->context, NULL);
    s->context->lpVtbl->IASetPrimitiveTopology(s->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s->context->lpVtbl->VSSetShader(s->context, s->quant_vs, NULL, 0);
    s->context->lpVtbl->VSSetConstantBuffers(s->context, 0, 1, &s->quant_const_buffer);
    s->context->lpVtbl->PSSetShaderResources(s->context, 0, 1, &s->quant_input_view);
    s->context->lpVtbl->PSSetSamplers(s->context, 0, 1, &s->quant_sampler);
    s->context->lpVtbl->PSSetConstantBuffers(s->context, 0, 1, &s->quant_const_buffer);

    viewport = (D3D11_VIEWPORT) {
        .TopLeftX = 0.0f,
        .TopLeftY = 0.0f,
        .Width = (float)s->width,
        .Height = (float)s->height,
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    };
    s->context->lpVtbl->RSSetViewports(s->context, 1, &viewport);
    s->context->lpVtbl->OMSetRenderTargets(s->context, 1, &y_rtv, NULL);
    s->context->lpVtbl->PSSetShader(s->context, s->quant_y_ps, NULL, 0);
    s->context->lpVtbl->Draw(s->context, 3, 0);
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 color quantization: Y plane drawn\n");

    viewport.Width = (float)(s->width / 2);
    viewport.Height = (float)(s->height / 2);
    s->context->lpVtbl->RSSetViewports(s->context, 1, &viewport);
    s->context->lpVtbl->OMSetRenderTargets(s->context, 1, &uv_rtv, NULL);
    s->context->lpVtbl->PSSetShader(s->context, s->quant_uv_ps, NULL, 0);
    s->context->lpVtbl->Draw(s->context, 3, 0);
    av_log(ctx, AV_LOG_VERBOSE, "D3D11 color quantization: UV plane drawn\n");

    s->context->lpVtbl->OMSetRenderTargets(s->context, 1, &null_rtv, NULL);
    s->context->lpVtbl->PSSetShaderResources(s->context, 0, 1, &null_srv);

    ret = 0;

fail:
    s->context->lpVtbl->OMSetRenderTargets(s->context, 1, &null_rtv, NULL);
    s->context->lpVtbl->PSSetShaderResources(s->context, 0, 1, &null_srv);
    if (uv_rtv)
        uv_rtv->lpVtbl->Release(uv_rtv);
    if (y_rtv)
        y_rtv->lpVtbl->Release(y_rtv);
    return ret;
}

static int scale_d3d11_configure_processor(ScaleD3D11Context *s, AVFilterContext *ctx) {
    HRESULT hr;

    switch (s->format) {
        case AV_PIX_FMT_NV12:
            s->output_format = DXGI_FORMAT_NV12;
            break;
        case AV_PIX_FMT_P010:
            s->output_format = DXGI_FORMAT_P010;
            break;
        default:
            av_log(ctx, AV_LOG_ERROR, "Invalid output format specified\n");
            return AVERROR(EINVAL);
    }

    if (s->color_bits && s->format != AV_PIX_FMT_NV12) {
        av_log(ctx, AV_LOG_ERROR, "color_bits supports only format=nv12\n");
        return AVERROR(EINVAL);
    }

    ///< Get D3D11 device and context from hardware device context
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;
    s->device = d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    av_log(ctx, AV_LOG_VERBOSE, "Configuring D3D11 video processor: %dx%d -> %dx%d\n",
           s->inputWidth, s->inputHeight, s->width, s->height);

    ///< Define the video processor content description
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputWidth = s->inputWidth,
        .InputHeight = s->inputHeight,
        .OutputWidth = s->width,
        .OutputHeight = s->height,
        .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
    };

    ///< Query video device interface
    hr = s->device->lpVtbl->QueryInterface(s->device, &IID_ID3D11VideoDevice, (void **)&s->videoDevice);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get D3D11 video device interface: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    ///< Create video processor enumerator
    hr = s->videoDevice->lpVtbl->CreateVideoProcessorEnumerator(s->videoDevice, &contentDesc, &s->enumerator);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor enumerator: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    ///< Create the video processor
    hr = s->videoDevice->lpVtbl->CreateVideoProcessor(s->videoDevice, s->enumerator, 0, &s->processor);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create video processor: HRESULT 0x%lX\n", hr);
        return AVERROR_EXTERNAL;
    }

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 video processor successfully configured\n");
    return 0;
}

static int scale_d3d11_filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ScaleD3D11Context *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    ID3D11VideoProcessorInputView *inputView = NULL;
    ID3D11VideoContext *videoContext = NULL;
    ID3D11Texture2D *output_texture = NULL;
    AVFrame *out = NULL;
    int ret = 0;
    HRESULT hr;

    ///< Validate input frame
    if (!in) {
        av_log(ctx, AV_LOG_ERROR, "Null input frame\n");
        return AVERROR(EINVAL);
    }

    if (!in->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hardware frames context in input frame\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    ///< Verify hardware device contexts
    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)in->hw_frames_ctx->data;

    if (!s->hw_device_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Filter hardware device context is uninitialized\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    AVHWDeviceContext *input_device_ctx = (AVHWDeviceContext *)frames_ctx->device_ref->data;
    AVHWDeviceContext *filter_device_ctx = (AVHWDeviceContext *)s->hw_device_ctx->data;

    if (input_device_ctx->type != filter_device_ctx->type) {
        av_log(ctx, AV_LOG_ERROR, "Mismatch between input and filter hardware device types\n");
        av_frame_free(&in);
        return AVERROR(EINVAL);
    }

    ///< Allocate output frame
    out = av_frame_alloc();
    if (!out) {
        av_log(ctx, AV_LOG_ERROR, "Failed to allocate output frame\n");
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    ret = av_hwframe_get_buffer(s->hw_frames_ctx_out, out, 0);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get output frame from pool\n");
        goto fail;
    }
    output_texture = (ID3D11Texture2D *)out->data[0];

    ///< Configure the D3D11 video processor if not already configured
    if (!s->color_bits && !s->processor) {
        ///< Get info from input texture
        D3D11_TEXTURE2D_DESC textureDesc;
        ID3D11Texture2D *input_texture = (ID3D11Texture2D *)in->data[0];
        input_texture->lpVtbl->GetDesc(input_texture, &textureDesc);

        s->inputWidth = textureDesc.Width;
        s->inputHeight = textureDesc.Height;
        s->input_format = textureDesc.Format;

        ret = scale_d3d11_configure_processor(s, ctx);
        if (ret < 0) {
            av_log(ctx, AV_LOG_ERROR, "Failed to configure processor\n");
            goto fail;
        }
    }

    ///< Get input texture and prepare input view
    ID3D11Texture2D *d3d11_texture = (ID3D11Texture2D *)in->data[0];
    int subIdx = (int)(intptr_t)in->data[1];

    if (s->color_bits) {
        ret = scale_d3d11_quantize_frame(s, ctx, in, out);
        if (ret < 0)
            goto fail;
        goto frame_ready;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {
        .FourCC = s->input_format,
        .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
        .Texture2D.ArraySlice = subIdx
    };

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorInputView(
        s->videoDevice, (ID3D11Resource *)d3d11_texture, s->enumerator, &inputViewDesc, &inputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create input view: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ///< Create output view for current texture
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {
        .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
        .Texture2D = { .MipSlice = 0 },
    };

    hr = s->videoDevice->lpVtbl->CreateVideoProcessorOutputView(
        s->videoDevice, (ID3D11Resource *)output_texture, s->enumerator, &outputViewDesc, &s->outputView);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to create output view: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    ///< Set up processing stream
    D3D11_VIDEO_PROCESSOR_STREAM stream = {
        .Enable = TRUE,
        .pInputSurface = inputView,
        .OutputIndex = 0
    };

    ///< Get video context
    hr = s->context->lpVtbl->QueryInterface(s->context, &IID_ID3D11VideoContext, (void **)&videoContext);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get video context: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

    if (frames_ctx->sw_format == AV_PIX_FMT_BGRA ||
        frames_ctx->sw_format == AV_PIX_FMT_X2BGR10 ||
        frames_ctx->sw_format == AV_PIX_FMT_RGBAF16) {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color_space = {
            .Usage = 0,
            .RGB_Range = 0,
            .YCbCr_Matrix = 1,
            .YCbCr_xvYCC = 0,
            .Nominal_Range = 2,
        };
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color_space = {
            .Usage = 0,
            .RGB_Range = 0,
            .YCbCr_Matrix = 1,
            .YCbCr_xvYCC = 0,
            .Nominal_Range = 1,
        };

        videoContext->lpVtbl->VideoProcessorSetStreamColorSpace(videoContext, s->processor,
                                                                 0, &input_color_space);
        videoContext->lpVtbl->VideoProcessorSetOutputColorSpace(videoContext, s->processor,
                                                                 &output_color_space);
    }

    ///< Process the frame
    hr = videoContext->lpVtbl->VideoProcessorBlt(videoContext, s->processor, s->outputView, 0, 1, &stream);
    if (FAILED(hr)) {
        av_log(ctx, AV_LOG_ERROR, "VideoProcessorBlt failed: HRESULT 0x%lX\n", hr);
        ret = AVERROR_EXTERNAL;
        goto fail;
    }

frame_ready:
    ///< Set up output frame
    ret = av_frame_copy_props(out, in);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to copy frame properties\n");
        goto fail;
    }

    out->data[0] = (uint8_t *)output_texture;
    out->data[1] = (uint8_t *)(intptr_t)0;
    out->width = s->width;
    out->height = s->height;
    out->format = AV_PIX_FMT_D3D11;
    if (s->format == AV_PIX_FMT_NV12 || s->format == AV_PIX_FMT_P010) {
        out->color_range     = AVCOL_RANGE_MPEG;
        out->color_primaries = AVCOL_PRI_BT709;
        out->color_trc       = AVCOL_TRC_BT709;
        out->colorspace      = AVCOL_SPC_BT709;
    }

    ///< Clean up resources
    if (inputView)
        inputView->lpVtbl->Release(inputView);
    if (videoContext)
        videoContext->lpVtbl->Release(videoContext);
    if (s->outputView) {
        s->outputView->lpVtbl->Release(s->outputView);
        s->outputView = NULL;
    }
    av_frame_free(&in);

    ///< Forward the frame
    return ff_filter_frame(outlink, out);

fail:
    if (inputView)
        inputView->lpVtbl->Release(inputView);
    if (videoContext)
        videoContext->lpVtbl->Release(videoContext);
    if (s->outputView) {
        s->outputView->lpVtbl->Release(s->outputView);
        s->outputView = NULL;
    }
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static int scale_d3d11_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ScaleD3D11Context *s = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];
    FilterLink *inl = ff_filter_link(inlink);
    FilterLink *outl = ff_filter_link(outlink);
    int ret;

    ///< Clean up any previous resources
    release_d3d11_resources(s);

    ///< Evaluate output dimensions
    ret = ff_scale_eval_dimensions(s, s->w_expr, s->h_expr, inlink, outlink, &s->width, &s->height);
    if (ret < 0) {
        av_log(ctx, AV_LOG_ERROR, "Failed to evaluate dimensions\n");
        return ret;
    }

    outlink->w = s->width;
    outlink->h = s->height;

    ///< Validate input hw_frames_ctx
    if (!inl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw_frames_ctx available on input link\n");
        return AVERROR(EINVAL);
    }

    ///< Propagate hw_frames_ctx to output
    outl->hw_frames_ctx = av_buffer_ref(inl->hw_frames_ctx);
    if (!outl->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "Failed to propagate hw_frames_ctx to output\n");
        return AVERROR(ENOMEM);
    }

    ///< Initialize filter's hardware device context
    if (!s->hw_device_ctx) {
        AVHWFramesContext *in_frames_ctx = (AVHWFramesContext *)inl->hw_frames_ctx->data;
        s->hw_device_ctx = av_buffer_ref(in_frames_ctx->device_ref);
        if (!s->hw_device_ctx) {
            av_log(ctx, AV_LOG_ERROR, "Failed to initialize filter hardware device context\n");
            return AVERROR(ENOMEM);
        }
    }

    ///< Get D3D11 device and context (but don't initialize processor yet - done in filter_frame)
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)s->hw_device_ctx->data;
    AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext *)hwctx->hwctx;

    s->device = d3d11_hwctx->device;
    s->context = d3d11_hwctx->device_context;

    if (!s->device || !s->context) {
        av_log(ctx, AV_LOG_ERROR, "Failed to get valid D3D11 device or context\n");
        return AVERROR(EINVAL);
    }

    ///< Create new hardware frames context for output
    s->hw_frames_ctx_out = av_hwframe_ctx_alloc(s->hw_device_ctx);
    if (!s->hw_frames_ctx_out)
        return AVERROR(ENOMEM);

    AVHWFramesContext *frames_ctx = (AVHWFramesContext *)s->hw_frames_ctx_out->data;
    frames_ctx->format = AV_PIX_FMT_D3D11;
    frames_ctx->sw_format = s->format;
    frames_ctx->width = s->width;
    frames_ctx->height = s->height;
    frames_ctx->initial_pool_size = 0;

    AVD3D11VAFramesContext *frames_hwctx = frames_ctx->hwctx;
    frames_hwctx->MiscFlags = 0;
    frames_hwctx->BindFlags = D3D11_BIND_RENDER_TARGET;

    ret = av_hwframe_ctx_init(s->hw_frames_ctx_out);
    if (ret < 0) {
        av_buffer_unref(&s->hw_frames_ctx_out);
        return ret;
    }

    outl->hw_frames_ctx = av_buffer_ref(s->hw_frames_ctx_out);
    if (!outl->hw_frames_ctx)
        return AVERROR(ENOMEM);

    av_log(ctx, AV_LOG_VERBOSE, "D3D11 scale config: %dx%d -> %dx%d\n",
           inlink->w, inlink->h, outlink->w, outlink->h);
    return 0;
}

static av_cold void scale_d3d11_uninit(AVFilterContext *ctx) {
    ScaleD3D11Context *s = ctx->priv;

    ///< Release D3D11 resources
    release_d3d11_resources(s);

    ///< Free the hardware device context reference
    av_buffer_unref(&s->hw_frames_ctx_out);
    av_buffer_unref(&s->hw_device_ctx);

    ///< Free option strings
    av_freep(&s->w_expr);
    av_freep(&s->h_expr);
}

static const AVFilterPad scale_d3d11_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = scale_d3d11_filter_frame,
    },
};

static const AVFilterPad scale_d3d11_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = scale_d3d11_config_props,
    },
};

#define OFFSET(x) offsetof(ScaleD3D11Context, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption scale_d3d11_options[] = {
    { "width",  "Output video width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, {.str = "iw"}, .flags = FLAGS },
    { "height", "Output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, {.str = "ih"}, .flags = FLAGS },
    { "format", "Output video pixel format", OFFSET(format), AV_OPT_TYPE_PIXEL_FMT, { .i64 = AV_PIX_FMT_NONE }, INT_MIN, INT_MAX, .flags=FLAGS },
    { "color_bits", "Quantize RGB before NV12 conversion: 0=disabled, 16=RGB565, 8=RGB332",
        OFFSET(color_bits), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 16, .flags=FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(scale_d3d11);

const FFFilter ff_vf_scale_d3d11 = {
    .p.name           = "scale_d3d11",
    .p.description    = NULL_IF_CONFIG_SMALL("Scale video using Direct3D11"),
    .priv_size        = sizeof(ScaleD3D11Context),
    .p.priv_class     = &scale_d3d11_class,
    .init             = scale_d3d11_init,
    .uninit           = scale_d3d11_uninit,
    FILTER_INPUTS(scale_d3d11_inputs),
    FILTER_OUTPUTS(scale_d3d11_outputs),
    FILTER_SINGLE_PIXFMT(AV_PIX_FMT_D3D11),
    .p.flags          = AVFILTER_FLAG_HWDEVICE,
    .flags_internal   = FF_FILTER_FLAG_HWFRAME_AWARE,
};
