/*
Copyright(c) 2016-2019 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES ==============================
#include "Renderer.h"
#include "Material.h"
#include "Model.h"
#include "ShaderBuffered.h"
#include "Deferred/ShaderVariation.h"
#include "Deferred/ShaderLight.h"
#include "Gizmos/Grid.h"
#include "Gizmos/Transform_Gizmo.h"
#include "Font/Font.h"
#include "../Profiling/Profiler.h"
#include "../Resource/IResource.h"
#include "../RHI/RHI_Device.h"
#include "../RHI/RHI_VertexBuffer.h"
#include "../RHI/RHI_ConstantBuffer.h"
#include "../RHI/RHI_Texture.h"
#include "../RHI/RHI_Sampler.h"
#include "../RHI/RHI_CommandList.h"
#include "../World/Entity.h"
#include "../World/Components/Renderable.h"
#include "../World/Components/Transform.h"
#include "../World/Components/Skybox.h"
#include "../World/Components/Light.h"
#include "../World/Components/Camera.h"
#include "../RHI/RHI_Texture2D.h"
//=========================================

//= NAMESPACES ================
using namespace std;
using namespace Spartan::Math;
using namespace Helper;
//=============================

static const float GIZMO_MAX_SIZE = 5.0f;
static const float GIZMO_MIN_SIZE = 0.1f;

namespace Spartan
{
	void Renderer::Pass_Main()
	{
#ifdef API_GRAPHICS_VULKAN
		return;
#endif
		m_cmd_list->Begin("Pass_Main");

		Pass_LightDepth();
		Pass_GBuffer();
		Pass_PreLight
		(
			m_render_tex_half_spare,	// IN:	
			m_render_tex_half_shadows,	// OUT: Shadows
			m_render_tex_half_ssao		// OUT: DO
		);
		Pass_Light
		(
			m_render_tex_half_shadows,	// IN:	Shadows
			m_render_tex_half_ssao,		// IN:	SSAO
			m_render_tex_full_hdr_light	// Out: Result
		);
		Pass_Transparent(m_render_tex_full_hdr_light);
		Pass_PostLight
		(
			m_render_tex_full_hdr_light,	// IN:	Light pass result
			m_render_tex_full_hdr_light2	// OUT: Result
		);
		Pass_Lines(m_render_tex_full_hdr_light2);
		Pass_Gizmos(m_render_tex_full_hdr_light2);
		Pass_DebugBuffer(m_render_tex_full_hdr_light2);
		Pass_PerformanceMetrics(m_render_tex_full_hdr_light2);

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_LightDepth()
	{
		uint32_t light_directional_count = 0;

		auto& light_entities = m_entities[Renderable_Light];
		for (const auto& light_entity : light_entities)
		{
			auto& light = light_entity->GetComponent<Light>();

			// Skip if it doesn't need to cast shadows
			if (!light->GetCastShadows())
				continue;

			// Acquire light's shadow map
			auto& shadow_map = light->GetShadowMap();
			if (!shadow_map)
				continue;

			// Get opaque renderable entities
			auto& entities = m_entities[Renderable_ObjectOpaque];
			if (entities.empty())
				continue;

			// Begin command list
			m_cmd_list->Begin("Pass_LightDepth");
			m_cmd_list->SetShaderPixel(nullptr);
			m_cmd_list->SetBlendState(m_blend_disabled);
			m_cmd_list->SetDepthStencilState(m_depth_stencil_enabled);
			m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
			m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
			m_cmd_list->SetShaderVertex(m_v_depth);
			m_cmd_list->SetInputLayout(m_v_depth->GetInputLayout());
			m_cmd_list->SetViewport(shadow_map->GetViewport());

			// Tracking
			uint32_t currently_bound_geometry = 0;

			for (uint32_t i = 0; i < light->GetShadowMap()->GetArraySize(); i++)
			{
				auto cascade_depth_stencil = shadow_map->GetResource_DepthStencil(i);

				m_cmd_list->Begin("Array_" + to_string(i + 1));
				m_cmd_list->ClearDepthStencil(cascade_depth_stencil, Clear_Depth, GetClearDepth());
				m_cmd_list->SetRenderTarget(nullptr, cascade_depth_stencil);

				Matrix light_view_projection = light->GetViewMatrix(i) * light->GetProjectionMatrix(i);

				for (const auto& entity : entities)
				{
					// Acquire renderable component
					auto renderable = entity->GetRenderable_PtrRaw();
					if (!renderable)
						continue;

					// Acquire material
					auto material = renderable->MaterialPtr();
					if (!material)
						continue;

					// Acquire geometry
					auto model = renderable->GeometryModel();
					if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
						continue;

					// Skip meshes that don't cast shadows
					if (!renderable->GetCastShadows())
						continue;

					// Skip transparent meshes (for now)
					if (material->GetColorAlbedo().w < 1.0f)
						continue;

					// Bind geometry
					if (currently_bound_geometry != model->GetResourceId())
					{
						m_cmd_list->SetBufferIndex(model->GetIndexBuffer());
						m_cmd_list->SetBufferVertex(model->GetVertexBuffer());
						currently_bound_geometry = model->GetResourceId();
					}

					// Accumulate directional light direction
					if (light->GetLightType() == LightType_Directional)
					{
						m_directional_light_avg_dir += light->GetDirection();
						light_directional_count++;
					}

					// Update constant buffer
					Transform* transform = entity->GetTransform_PtrRaw();
					transform->UpdateConstantBufferLight(m_rhi_device, light_view_projection, i);
					m_cmd_list->SetConstantBuffer(1, Buffer_VertexShader, transform->GetConstantBufferLight(i));

					m_cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
				}
				m_cmd_list->End(); // end of cascade
			}
			m_cmd_list->End();
			m_cmd_list->Submit();
		}

		// Compute average directional light direction
		m_directional_light_avg_dir /= static_cast<float>(light_directional_count);
	}

	void Renderer::Pass_GBuffer()
	{
		if (!m_rhi_device)
			return;

		m_cmd_list->Begin("Pass_GBuffer");

		Vector4 clear_color	= Vector4::Zero;
		
		// If there is nothing to render, just clear
		if (m_entities[Renderable_ObjectOpaque].empty())
		{
			m_cmd_list->ClearRenderTarget(m_g_buffer_albedo->GetResource_RenderTarget(), Vector4::Zero);
			m_cmd_list->ClearRenderTarget(m_g_buffer_normal->GetResource_RenderTarget(), Vector4::Zero);
			m_cmd_list->ClearRenderTarget(m_g_buffer_material->GetResource_RenderTarget(), Vector4::Zero); // zeroed material buffer causes sky sphere to render
			m_cmd_list->ClearRenderTarget(m_g_buffer_velocity->GetResource_RenderTarget(), Vector4::Zero);
			m_cmd_list->ClearDepthStencil(m_g_buffer_depth->GetResource_DepthStencil(), Clear_Depth, GetClearDepth());
			m_cmd_list->End();
			m_cmd_list->Submit();
			return;
		}

		// Prepare resources
		SetDefaultBuffer(static_cast<uint32_t>(m_resolution.x), static_cast<uint32_t>(m_resolution.y));
		vector<void*> textures(8);
		vector<void*> render_targets
		{
			m_g_buffer_albedo->GetResource_RenderTarget(),
			m_g_buffer_normal->GetResource_RenderTarget(),
			m_g_buffer_material->GetResource_RenderTarget(),
			m_g_buffer_velocity->GetResource_RenderTarget()
		};
	
		// Star command list
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetDepthStencilState(m_depth_stencil_enabled);
		m_cmd_list->SetViewport(m_g_buffer_albedo->GetViewport());
		m_cmd_list->SetRenderTargets(render_targets, m_g_buffer_depth->GetResource_DepthStencil());
		m_cmd_list->ClearRenderTargets(render_targets, clear_color);
		m_cmd_list->ClearDepthStencil(m_g_buffer_depth->GetResource_DepthStencil(), Clear_Depth, GetClearDepth());
		m_cmd_list->SetShaderVertex(m_vs_gbuffer);
		m_cmd_list->SetInputLayout(m_vs_gbuffer->GetInputLayout());
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->SetSampler(0, m_sampler_anisotropic_wrap);	
		
		// Variables that help reduce state changes
		uint32_t currently_bound_geometry	= 0;
		uint32_t currently_bound_shader		= 0;
		uint32_t currently_bound_material	= 0;

		for (auto entity : m_entities[Renderable_ObjectOpaque])
		{
			// Get renderable and material
			auto renderable = entity->GetRenderable_PtrRaw();
			auto material	= renderable ? renderable->MaterialPtr().get() : nullptr;

			if (!renderable || !material)
				continue;

			// Get shader and geometry
			auto shader = material->GetShader();
			auto model	= renderable->GeometryModel();

			// Validate shader
			if (!shader || shader->GetCompilationState() != Shader_Compiled)
				continue;

			// Validate geometry
			if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
				continue;

			// Skip objects outside of the view frustum
			if (!m_camera->IsInViewFrustrum(renderable))
				continue;

			// Set face culling (changes only if required)
			m_cmd_list->SetRasterizerState(GetRasterizerState(material->GetCullMode(), Fill_Solid));

			// Bind geometry
			if (currently_bound_geometry != model->GetResourceId())
			{
				m_cmd_list->SetBufferIndex(model->GetIndexBuffer());
				m_cmd_list->SetBufferVertex(model->GetVertexBuffer());
				currently_bound_geometry = model->GetResourceId();
			}

			// Bind shader
			if (currently_bound_shader != shader->RHI_GetID())
			{
				m_cmd_list->SetShaderPixel(static_pointer_cast<RHI_Shader>(shader));
				currently_bound_shader = shader->RHI_GetID();
			}

			// Bind material
			if (currently_bound_material != material->GetResourceId())
			{
				// Bind material textures
				textures[0] = material->GetTextureShaderResourceByType(TextureType_Albedo);
				textures[1] = material->GetTextureShaderResourceByType(TextureType_Roughness);
				textures[2] = material->GetTextureShaderResourceByType(TextureType_Metallic);
				textures[3] = material->GetTextureShaderResourceByType(TextureType_Normal);
				textures[4] = material->GetTextureShaderResourceByType(TextureType_Height);
				textures[5] = material->GetTextureShaderResourceByType(TextureType_Occlusion);
				textures[6] = material->GetTextureShaderResourceByType(TextureType_Emission);
				textures[7] = material->GetTextureShaderResourceByType(TextureType_Mask);
				m_cmd_list->SetTextures(0, textures);

				// Bind material buffer
				material->UpdateConstantBuffer();
				m_cmd_list->SetConstantBuffer(1, Buffer_PixelShader, material->GetConstantBuffer());

				currently_bound_material = material->GetResourceId();
			}

			// Bind object buffer
			Transform* transform = entity->GetTransform_PtrRaw();
			transform->UpdateConstantBuffer(m_rhi_device, m_view_projection);
			m_cmd_list->SetConstantBuffer(2, Buffer_VertexShader, transform->GetConstantBuffer());

			// Render	
			m_cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());
			m_profiler->m_renderer_meshes_rendered++;

		} // ENTITY/MESH ITERATION

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_PreLight(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_shadows_out, shared_ptr<RHI_Texture>& tex_ssao_out)
	{
		m_cmd_list->Begin("Pass_PreLight");
		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
		m_cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		m_cmd_list->ClearRenderTarget(tex_shadows_out->GetResource_RenderTarget(), Vector4::One);

		// SHADOW MAPPING + BLUR
		auto shadow_mapped = false;
		auto& lights = m_entities[Renderable_Light];
		for (uint32_t i = 0; i < lights.size(); i++)
		{
			auto light = lights[i]->GetComponent<Light>().get();

			// Skip lights that don't cast shadows
			if (!light->GetCastShadows())
				continue;

			Pass_ShadowMapping(tex_shadows_out, light);
			shadow_mapped = true;
		}
		if (!shadow_mapped)
		{
			m_cmd_list->ClearRenderTarget(tex_shadows_out->GetResource_RenderTarget(), Vector4::One);
		}

		// SSAO MAPPING + BLUR
		if (m_flags & Render_PostProcess_SSAO)
		{
			Pass_SSAO(tex_in);
			const auto sigma		= 1.0f;
			const auto pixel_stride	= 1.0f;
			Pass_BlurBilateralGaussian(tex_in, tex_ssao_out, sigma, pixel_stride);
		}

		m_cmd_list->End();
	}

	void Renderer::Pass_Light(shared_ptr<RHI_Texture>& tex_shadows, shared_ptr<RHI_Texture>& tex_ssao, shared_ptr<RHI_Texture>& tex_out)
	{
		if (m_vps_light->GetCompilationState() != Shader_Compiled)
			return;

		m_cmd_list->Begin("Pass_Light");

		// Update constant buffers
		SetDefaultBuffer(static_cast<uint32_t>(m_resolution.x), static_cast<uint32_t>(m_resolution.y));
		m_vps_light->UpdateConstantBuffer
		(
			m_view_projection_orthographic,
			m_view,
			m_projection,
			m_entities[Renderable_Light],
			Flags_IsSet(Render_PostProcess_SSR)
		);

		// Prepare resources
		auto shader						= static_pointer_cast<RHI_Shader>(m_vps_light);
		vector<void*> samplers			= { m_sampler_trilinear_clamp->GetResource(), m_sampler_point_clamp->GetResource() };
		vector<void*> constant_buffers	= { m_buffer_global->GetResource(),  m_vps_light->GetConstantBuffer()->GetResource() };
		vector<void*> textures =
		{
			m_g_buffer_albedo->GetResource_Texture(),																		// Albedo	
			m_g_buffer_normal->GetResource_Texture(),																		// Normal
			m_g_buffer_depth->GetResource_Texture(),																		// Depth
			m_g_buffer_material->GetResource_Texture(),																		// Material
			tex_shadows->GetResource_Texture(),																				// Shadows
			Flags_IsSet(Render_PostProcess_SSAO) ? tex_ssao->GetResource_Texture() : m_tex_white->GetResource_Texture(),	// SSAO
			m_render_tex_full_hdr_light2->GetResource_Texture(),															// Previous frame
			m_skybox ? m_skybox->GetTexture()->GetResource_Texture() : m_tex_white->GetResource_Texture(),					// Environment
			m_tex_lut_ibl->GetResource_Texture()																			// LutIBL
		};

		// Setup command list
		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetShaderVertex(shader);
		m_cmd_list->SetShaderPixel(shader);
		m_cmd_list->SetInputLayout(shader->GetInputLayout());
		m_cmd_list->SetSamplers(0, samplers);
		m_cmd_list->SetTextures(0, textures);
		m_cmd_list->SetConstantBuffers(0, Buffer_Global, constant_buffers);
		m_cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		m_cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_Transparent(shared_ptr<RHI_Texture>& tex_out)
	{
		auto& entities_transparent = m_entities[Renderable_ObjectTransparent];
		if (entities_transparent.empty())
			return;

		// Prepare resources
		vector<void*> textures = { m_g_buffer_depth->GetResource_Texture(), m_skybox ? m_skybox->GetTexture()->GetResource_Texture() : nullptr };

		// Begin command list
		m_cmd_list->Begin("Pass_Transparent");
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetBlendState(m_blend_enabled);	
		m_cmd_list->SetDepthStencilState(m_depth_stencil_enabled);
		m_cmd_list->SetRenderTarget(tex_out, m_g_buffer_depth->GetResource_DepthStencil());
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetTextures(0, textures);
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetShaderVertex(m_vps_transparent);
		m_cmd_list->SetInputLayout(m_vps_transparent->GetInputLayout());
		m_cmd_list->SetShaderPixel(m_vps_transparent);

		for (auto& entity : entities_transparent)
		{
			// Get renderable and material
			auto renderable	= entity->GetRenderable_PtrRaw();
			auto material	= renderable ? renderable->MaterialPtr().get() : nullptr;

			if (!renderable || !material)
				continue;

			// Get geometry
			auto model = renderable->GeometryModel();
			if (!model || !model->GetVertexBuffer() || !model->GetIndexBuffer())
				continue;

			// Skip objects outside of the view frustum
			if (!m_camera->IsInViewFrustrum(renderable))
				continue;

			// Set the following per object
			m_cmd_list->SetRasterizerState(GetRasterizerState(material->GetCullMode(), Fill_Solid));
			m_cmd_list->SetBufferIndex(model->GetIndexBuffer());
			m_cmd_list->SetBufferVertex(model->GetVertexBuffer());

			// Constant buffer - TODO: Make per object
			auto buffer = Struct_Transparency
			(
				entity->GetTransform_PtrRaw()->GetMatrix(),
				m_view,
				m_projection,
				material->GetColorAlbedo(),
				m_camera->GetTransform()->GetPosition(),
				m_directional_light_avg_dir,
				material->GetRoughnessMultiplier()
			);
			m_vps_transparent->UpdateBuffer(&buffer);
			m_cmd_list->SetConstantBuffer(1, Buffer_Global, m_vps_transparent->GetConstantBuffer());
			m_cmd_list->DrawIndexed(renderable->GeometryIndexCount(), renderable->GeometryIndexOffset(), renderable->GeometryVertexOffset());

			m_profiler->m_renderer_meshes_rendered++;

		} // ENTITY/MESH ITERATION

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_ShadowMapping(shared_ptr<RHI_Texture>& tex_out, Light* light)
	{
		if (!light || !light->GetCastShadows())
			return;

		m_cmd_list->Begin("Pass_ShadowMapping");

		// Get appropriate pixel shader
		shared_ptr<ShaderBuffered> pixel_shader;
		if (light->GetLightType() == LightType_Directional)
		{
			pixel_shader = m_vps_shadow_mapping_directional;
		}
		else if (light->GetLightType() == LightType_Point)
		{
			pixel_shader = m_ps_shadow_mapping_point;
		}
		else if (light->GetLightType() == LightType_Spot)
		{
			pixel_shader = m_ps_shadow_mapping_spot;
		}
		
		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight(), m_view_projection_orthographic);
		auto buffer = Struct_ShadowMapping((m_view_projection).Inverted(), light);
		pixel_shader->UpdateBuffer(&buffer);
		vector<void*> constant_buffers	= { m_buffer_global->GetResource(), pixel_shader->GetConstantBuffer()->GetResource() };
		vector<void*> samplers			= { m_sampler_compare_depth->GetResource(), m_sampler_bilinear_clamp->GetResource() };
		vector<void*> textures =
		{
			m_g_buffer_normal->GetResource_Texture(),
			m_g_buffer_depth->GetResource_Texture(),
			light->GetLightType() == LightType_Directional	? light->GetShadowMap()->GetResource_Texture() : nullptr,
			light->GetLightType() == LightType_Point		? light->GetShadowMap()->GetResource_Texture() : nullptr,
			light->GetLightType() == LightType_Spot			? light->GetShadowMap()->GetResource_Texture() : nullptr
		};

		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetBlendState(m_blend_shadow_maps);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderVertex(m_vps_shadow_mapping_directional);
		m_cmd_list->SetInputLayout(m_vps_shadow_mapping_directional->GetInputLayout());
		m_cmd_list->SetShaderPixel(pixel_shader);
		m_cmd_list->SetTextures(0, textures);
		m_cmd_list->SetSamplers(0, samplers);
		m_cmd_list->SetConstantBuffers(0, Buffer_Global, constant_buffers);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_PostLight(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		// All post-process passes share the following, so set them once here
		m_cmd_list->Begin("Pass_PostLight");
		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
		m_cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		m_cmd_list->SetShaderVertex(m_vs_quad);
		m_cmd_list->SetInputLayout(m_vs_quad->GetInputLayout());

		// Render target swapping
		const auto swap_targets = [this, &tex_in, &tex_out]() { m_cmd_list->Submit(); tex_out.swap(tex_in); };

		// TAA	
		if (Flags_IsSet(Render_PostProcess_TAA))
		{
			Pass_TAA(tex_in, tex_out);
			swap_targets();
		}

		// Bloom
		if (Flags_IsSet(Render_PostProcess_Bloom))
		{
			Pass_Bloom(tex_in, tex_out);
			swap_targets();
		}

		// Motion Blur
		if (Flags_IsSet(Render_PostProcess_MotionBlur))
		{
			Pass_MotionBlur(tex_in, tex_out);
			swap_targets();
		}

		// Dithering
		if (Flags_IsSet(Render_PostProcess_Dithering))
		{
			Pass_Dithering(tex_in, tex_out);
			swap_targets();
		}

		// Tone-Mapping
		if (m_tonemapping != ToneMapping_Off)
		{
			Pass_ToneMapping(tex_in, tex_out);
			swap_targets();
		}

		// FXAA
		if (Flags_IsSet(Render_PostProcess_FXAA))
		{
			Pass_FXAA(tex_in, tex_out);
			swap_targets();
		}

		// Sharpening
		if (Flags_IsSet(Render_PostProcess_Sharpening))
		{
			Pass_Sharpening(tex_in, tex_out);
			swap_targets();
		}

		// Chromatic aberration
		if (Flags_IsSet(Render_PostProcess_ChromaticAberration))
		{
			Pass_ChromaticAberration(tex_in, tex_out);
			swap_targets();
		}

		// Gamma correction
		Pass_GammaCorrection(tex_in, tex_out);

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_SSAO(shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_SSAO");

		// Prepare resources
		vector<void*> textures = { m_g_buffer_normal->GetResource_Texture(), m_g_buffer_depth->GetResource_Texture(), m_tex_noise_normal->GetResource_Texture() };
		vector<void*> samplers = { m_sampler_bilinear_clamp->GetResource() /*SSAO (clamp) */, m_sampler_bilinear_wrap->GetResource() /*SSAO noise texture (wrap)*/};
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from some previous pass)
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetRenderTarget(tex_out);	
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderVertex(m_vs_quad);
		m_cmd_list->SetInputLayout(m_vs_quad->GetInputLayout());
		m_cmd_list->SetShaderPixel(m_vps_ssao);
		m_cmd_list->SetTextures(0, textures);
		m_cmd_list->SetSamplers(0, samplers);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_BlurBox(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma)
	{
		m_cmd_list->Begin("Pass_BlurBox");

		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_blur_box);
		m_cmd_list->SetTexture(0, tex_in); // Shadows are in the alpha channel
		m_cmd_list->SetSampler(0, m_sampler_trilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_BlurGaussian(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride)
	{
		if (tex_in->GetWidth() != tex_out->GetWidth() || tex_in->GetHeight() != tex_out->GetHeight() || tex_in->GetFormat() != tex_out->GetFormat())
		{
			LOG_ERROR("Invalid parameters, textures must match because they will get swapped");
			return;
		}

		SetDefaultBuffer(tex_in->GetWidth(), tex_in->GetHeight());

		// Start command list
		m_cmd_list->Begin("Pass_BlurBilateralGaussian");
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_blur_gaussian);
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);

		// Horizontal Gaussian blur	
		m_cmd_list->Begin("Pass_BlurBilateralGaussian_Horizontal");
		{
			auto direction	= Vector2(pixel_stride, 0.0f);
			auto buffer		= Struct_Blur(direction, sigma);
			m_ps_blur_gaussian->UpdateBuffer(&buffer, 0);

			m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
			m_cmd_list->SetRenderTarget(tex_out);
			m_cmd_list->SetTexture(0, tex_in);
			m_cmd_list->SetConstantBuffer(1, Buffer_PixelShader, m_ps_blur_gaussian_bilateral->GetConstantBuffer(0));
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		// Vertical Gaussian blur
		m_cmd_list->Begin("Pass_BlurBilateralGaussian_Horizontal");
		{
			auto direction	= Vector2(0.0f, pixel_stride);
			auto buffer		= Struct_Blur(direction, sigma);
			m_ps_blur_gaussian->UpdateBuffer(&buffer, 1);

			m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
			m_cmd_list->SetRenderTarget(tex_in);
			m_cmd_list->SetTexture(0, tex_out);
			m_cmd_list->SetConstantBuffer(1, Buffer_PixelShader, m_ps_blur_gaussian_bilateral->GetConstantBuffer(1));
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		m_cmd_list->End();
		m_cmd_list->Submit();

		// Swap textures
		tex_in.swap(tex_out);
	}

	void Renderer::Pass_BlurBilateralGaussian(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out, const float sigma, const float pixel_stride)
	{
		if (tex_in->GetWidth() != tex_out->GetWidth() || tex_in->GetHeight() != tex_out->GetHeight() || tex_in->GetFormat() != tex_out->GetFormat())
		{
			LOG_ERROR("Invalid parameters, textures must match because they will get swapped.");
			return;
		}

		SetDefaultBuffer(tex_in->GetWidth(), tex_in->GetHeight());

		// Start command list
		m_cmd_list->Begin("Pass_BlurBilateralGaussian");
		m_cmd_list->SetViewport(tex_out->GetViewport());	
		m_cmd_list->SetShaderVertex(m_vs_quad);
		m_cmd_list->SetInputLayout(m_vs_quad->GetInputLayout());
		m_cmd_list->SetShaderPixel(m_ps_blur_gaussian_bilateral);	
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);

		// Horizontal Gaussian blur
		m_cmd_list->Begin("Pass_BlurBilateralGaussian_Horizontal");
		{
			// Prepare resources
			auto direction	= Vector2(pixel_stride, 0.0f);
			auto buffer		= Struct_Blur(direction, sigma);
			m_ps_blur_gaussian_bilateral->UpdateBuffer(&buffer, 0);
			vector<void*> textures = { tex_in->GetResource_Texture(), m_g_buffer_depth->GetResource_Texture(), m_g_buffer_normal->GetResource_Texture() };
			
			m_cmd_list->ClearTextures(); // avoids d3d11 warning where render target is also bound as texture (from Pass_PreLight)
			m_cmd_list->SetRenderTarget(tex_out);
			m_cmd_list->SetTextures(0, textures);
			m_cmd_list->SetConstantBuffer(1, Buffer_PixelShader, m_ps_blur_gaussian_bilateral->GetConstantBuffer(0));
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		// Vertical Gaussian blur
		m_cmd_list->Begin("Pass_BlurBilateralGaussian_Vertical");
		{
			// Prepare resources
			auto direction	= Vector2(0.0f, pixel_stride);
			auto buffer		= Struct_Blur(direction, sigma);
			m_ps_blur_gaussian_bilateral->UpdateBuffer(&buffer, 1);
			vector<void*> textures = { tex_out->GetResource_Texture(), m_g_buffer_depth->GetResource_Texture(), m_g_buffer_normal->GetResource_Texture() };

			m_cmd_list->ClearTextures(); // avoids d3d11 warning where render target is also bound as texture (from above pass)
			m_cmd_list->SetRenderTarget(tex_in);
			m_cmd_list->SetTextures(0, textures);
			m_cmd_list->SetConstantBuffer(1, Buffer_PixelShader, m_ps_blur_gaussian_bilateral->GetConstantBuffer(1));
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		m_cmd_list->End();
		m_cmd_list->Submit();

		tex_in.swap(tex_out);
	}

	void Renderer::Pass_TAA(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_TAA");

		// Resolve
		{
			// Prepare resources
			SetDefaultBuffer(m_render_tex_full_taa_current->GetWidth(), m_render_tex_full_taa_current->GetHeight());
			vector<void*> textures = { m_render_tex_full_taa_history->GetResource_Texture(), tex_in->GetResource_Texture(), m_g_buffer_velocity->GetResource_Texture(), m_g_buffer_depth->GetResource_Texture() };

			m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from some previous pass)
			m_cmd_list->SetRenderTarget(m_render_tex_full_taa_current);
			m_cmd_list->SetViewport(m_render_tex_full_taa_current->GetViewport());
			m_cmd_list->SetShaderPixel(m_ps_taa);
			m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
			m_cmd_list->SetTextures(0, textures);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}

		// Output to texOut
		{
			// Prepare resources
			SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

			m_cmd_list->SetRenderTarget(tex_out);
			m_cmd_list->SetViewport(tex_out->GetViewport());
			m_cmd_list->SetShaderPixel(m_ps_texture);
			m_cmd_list->SetSampler(0, m_sampler_point_clamp);
			m_cmd_list->SetTexture(0, m_render_tex_full_taa_current);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}

		m_cmd_list->End();
		m_cmd_list->Submit();

		// Swap textures so current becomes history
		m_render_tex_full_taa_current.swap(m_render_tex_full_taa_history);
	}

	void Renderer::Pass_Bloom(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_Bloom");
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);

		m_cmd_list->Begin("Downsample");
		{
			// Prepare resources
			SetDefaultBuffer(m_render_tex_quarter_blur1->GetWidth(), m_render_tex_quarter_blur1->GetHeight());

			m_cmd_list->SetRenderTarget(m_render_tex_quarter_blur1);
			m_cmd_list->SetViewport(m_render_tex_quarter_blur1->GetViewport());
			m_cmd_list->SetShaderPixel(m_ps_downsample_box);
			m_cmd_list->SetTexture(0, tex_in);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		m_cmd_list->Begin("Luminance");
		{
			// Prepare resources
			SetDefaultBuffer(m_render_tex_quarter_blur2->GetWidth(), m_render_tex_quarter_blur2->GetHeight());

			m_cmd_list->SetRenderTarget(m_render_tex_quarter_blur2);
			m_cmd_list->SetViewport(m_render_tex_quarter_blur2->GetViewport());	
			m_cmd_list->SetShaderPixel(m_ps_bloom_bright);
			m_cmd_list->SetTexture(0, m_render_tex_quarter_blur1);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		// Gaussian blur
		const auto sigma = 2.0f;
		Pass_BlurGaussian(m_render_tex_quarter_blur2, m_render_tex_quarter_blur1, sigma);

		// Upsampling progressively yields the best results [Kraus2007]

		m_cmd_list->Begin("Upscale");
		{
			// Prepare resources
			SetDefaultBuffer(m_render_tex_half_spare2->GetWidth(), m_render_tex_half_spare2->GetHeight());

			m_cmd_list->SetRenderTarget(m_render_tex_half_spare2);
			m_cmd_list->SetViewport(m_render_tex_half_spare2->GetViewport());
			m_cmd_list->SetShaderPixel(m_ps_upsample_box);
			m_cmd_list->SetTexture(0, m_render_tex_quarter_blur2);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		m_cmd_list->Begin("Upscale");
		{
			// Prepare resources
			SetDefaultBuffer(m_render_tex_full_spare->GetWidth(), m_render_tex_full_spare->GetHeight());

			m_cmd_list->SetRenderTarget(m_render_tex_full_spare);
			m_cmd_list->SetViewport(m_render_tex_full_spare->GetViewport());
			m_cmd_list->SetShaderPixel(m_ps_upsample_box);
			m_cmd_list->SetTexture(0, m_render_tex_half_spare2);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		m_cmd_list->Begin("Additive_Blending");
		{
			// Prepare resources
			SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());
			vector<void*> textures = { tex_in->GetResource_Texture(), m_render_tex_full_spare->GetResource_Texture() };

			m_cmd_list->SetRenderTarget(tex_out);
			m_cmd_list->SetViewport(tex_out->GetViewport());
			m_cmd_list->SetShaderPixel(m_ps_bloom_blend);
			m_cmd_list->SetTextures(0, textures);
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
			m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		}
		m_cmd_list->End();

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_ToneMapping(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_ToneMapping");

		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_tone_mapping);
		m_cmd_list->SetTexture(0, tex_in);
		m_cmd_list->SetSampler(0, m_sampler_point_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_GammaCorrection(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_GammaCorrection");

		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_gamma_correction);
		m_cmd_list->SetTexture(0, tex_in);
		m_cmd_list->SetSampler(0, m_sampler_point_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_FXAA(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_FXAA");

		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);

		// Luma
		m_cmd_list->SetRenderTarget(tex_out);	
		m_cmd_list->SetShaderPixel(m_ps_luma);
		m_cmd_list->SetTexture(0, tex_in);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);

		// FXAA
		m_cmd_list->SetRenderTarget(tex_in);
		m_cmd_list->SetShaderPixel(m_ps_fxaa);
		m_cmd_list->SetTexture(0, tex_out);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);

		m_cmd_list->End();
		m_cmd_list->Submit();

		// Swap the textures
		tex_in.swap(tex_out);
	}

	void Renderer::Pass_ChromaticAberration(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_ChromaticAberration");

		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_chromatic_aberration);
		m_cmd_list->SetTexture(0, tex_in);
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_MotionBlur(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_MotionBlur");

		// Prepare resources
		vector<void*> textures = { tex_in->GetResource_Texture(), m_g_buffer_velocity->GetResource_Texture() };
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_motion_blur);
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetTextures(0, textures);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_Dithering(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_Dithering");

		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());

		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetShaderPixel(m_ps_dithering);
		m_cmd_list->SetSampler(0, m_sampler_point_clamp);
		m_cmd_list->SetTexture(0, tex_in);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_Sharpening(shared_ptr<RHI_Texture>& tex_in, shared_ptr<RHI_Texture>& tex_out)
	{
		m_cmd_list->Begin("Pass_Sharpening");

		// Prepare resources
		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight());
	
		m_cmd_list->ClearTextures(); // avoids d3d11 warning where the render target is already bound as an input texture (from previous pass)
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());		
		m_cmd_list->SetShaderPixel(m_ps_sharpening);
		m_cmd_list->SetTexture(0, tex_in);
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_Lines(shared_ptr<RHI_Texture>& tex_out)
	{
		const bool draw_picking_ray = m_flags & Render_Gizmo_PickingRay;
		const bool draw_aabb		= m_flags & Render_Gizmo_AABB;
		const bool draw_grid		= m_flags & Render_Gizmo_Grid;
		const auto draw_lines		= !m_lines_list_depth_enabled.empty() || !m_lines_list_depth_disabled.empty(); // Any kind of lines, physics, user debug, etc.
		const auto draw				= draw_picking_ray || draw_aabb || draw_grid || draw_lines;
		if (!draw)
			return;

		m_cmd_list->Begin("Pass_Lines");

		// Generate lines for debug primitives offered by the renderer
		{
			// Picking ray
			if (draw_picking_ray)
			{
				const Ray& ray = m_camera->GetPickingRay();
				DrawLine(ray.GetStart(), ray.GetStart() + ray.GetDirection() * m_camera->GetFarPlane(), Vector4(0, 1, 0, 1));
			}

			// AABBs
			if (draw_aabb)
			{
				for (const auto& entity : m_entities[Renderable_ObjectOpaque])
				{
					if (auto renderable = entity->GetRenderable_PtrRaw())
					{
						DrawBox(renderable->GeometryAabb(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
					}
				}

				for (const auto& entity : m_entities[Renderable_ObjectTransparent])
				{
					if (auto renderable = entity->GetRenderable_PtrRaw())
					{
						DrawBox(renderable->GeometryAabb(), Vector4(0.41f, 0.86f, 1.0f, 1.0f));
					}
				}
			}
		}

		// Begin command list
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_wireframe);
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_LineList);
		m_cmd_list->SetShaderVertex(m_vps_color);
		m_cmd_list->SetShaderPixel(m_vps_color);
		m_cmd_list->SetInputLayout(m_vps_color->GetInputLayout());
		m_cmd_list->SetSampler(0, m_sampler_point_clamp);
		
		// unjittered matrix to avoid TAA jitter due to lack of motion vectors (line rendering is anti-aliased by m_rasterizer_cull_back_wireframe, decently)
		const auto view_projection_unjittered = m_camera->GetViewMatrix() * m_camera->GetProjectionMatrix();

		// Draw lines that require depth
		m_cmd_list->SetDepthStencilState(m_depth_stencil_enabled);
		m_cmd_list->SetRenderTarget(tex_out, m_g_buffer_depth->GetResource_DepthStencil());
		{
			// Grid
			if (draw_grid)
			{
				SetDefaultBuffer
				(
					static_cast<uint32_t>(m_resolution.x),
					static_cast<uint32_t>(m_resolution.y),
					m_gizmo_grid->ComputeWorldMatrix(m_camera->GetTransform()) * view_projection_unjittered
				);
				m_cmd_list->SetBufferIndex(m_gizmo_grid->GetIndexBuffer());
				m_cmd_list->SetBufferVertex(m_gizmo_grid->GetVertexBuffer());
				m_cmd_list->SetBlendState(m_blend_enabled);
				m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
				m_cmd_list->DrawIndexed(m_gizmo_grid->GetIndexCount(), 0, 0);
			}

			// Lines
			const auto line_vertex_buffer_size = static_cast<uint32_t>(m_lines_list_depth_enabled.size());
			if (line_vertex_buffer_size != 0)
			{
				// Grow vertex buffer (if needed)
				if (line_vertex_buffer_size > m_vertex_buffer_lines->GetVertexCount())
				{
					m_vertex_buffer_lines->CreateDynamic<RHI_Vertex_PosCol>(line_vertex_buffer_size);
				}

				// Update vertex buffer
				const auto buffer = static_cast<RHI_Vertex_PosCol*>(m_vertex_buffer_lines->Map());
				copy(m_lines_list_depth_enabled.begin(), m_lines_list_depth_enabled.end(), buffer);
				m_vertex_buffer_lines->Unmap();

				SetDefaultBuffer(static_cast<uint32_t>(m_resolution.x), static_cast<uint32_t>(m_resolution.y), view_projection_unjittered);
				m_cmd_list->SetBufferVertex(m_vertex_buffer_lines);
				m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
				m_cmd_list->Draw(line_vertex_buffer_size);

				m_lines_list_depth_enabled.clear();
			}
		}

		// Draw lines that don't require depth
		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRenderTarget(tex_out);
		{
			// Lines
			const auto line_vertex_buffer_size = static_cast<uint32_t>(m_lines_list_depth_disabled.size());
			if (line_vertex_buffer_size != 0)
			{
				// Grow vertex buffer (if needed)
				if (line_vertex_buffer_size > m_vertex_buffer_lines->GetVertexCount())
				{
					m_vertex_buffer_lines->CreateDynamic<RHI_Vertex_PosCol>(line_vertex_buffer_size);
				}

				// Update vertex buffer
				const auto buffer = static_cast<RHI_Vertex_PosCol*>(m_vertex_buffer_lines->Map());
				copy(m_lines_list_depth_disabled.begin(), m_lines_list_depth_disabled.end(), buffer);
				m_vertex_buffer_lines->Unmap();

				// Set pipeline state
				m_cmd_list->SetBufferVertex(m_vertex_buffer_lines);
				SetDefaultBuffer(static_cast<uint32_t>(m_resolution.x), static_cast<uint32_t>(m_resolution.y), view_projection_unjittered);
				m_cmd_list->Draw(line_vertex_buffer_size);

				m_lines_list_depth_disabled.clear();
			}
		}

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_Gizmos(shared_ptr<RHI_Texture>& tex_out)
	{
		bool render_lights		= m_flags & Render_Gizmo_Lights;
		bool render_transform	= m_flags & Render_Gizmo_Transform;
		bool render				= render_lights || render_transform;
		if (!render)
			return;

		m_cmd_list->Begin("Pass_Gizmos");
		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetBlendState(m_blend_enabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetViewport(tex_out->GetViewport());	
		m_cmd_list->SetRenderTarget(tex_out);

		auto& lights = m_entities[Renderable_Light];
		if (render_lights && !lights.empty())
		{
			m_cmd_list->Begin("Pass_Gizmos_Lights");

			for (const auto& entity : lights)
			{
				auto position_light_world		= entity->GetTransform_PtrRaw()->GetPosition();
				auto position_camera_world		= m_camera->GetTransform()->GetPosition();
				auto direction_camera_to_light	= (position_light_world - position_camera_world).Normalized();
				auto v_dot_l					= Vector3::Dot(m_camera->GetTransform()->GetForward(), direction_camera_to_light);

				// Don't bother drawing if out of view
				if (v_dot_l <= 0.5f)
					continue;

				// Compute light screen space position and scale (based on distance from the camera)
				auto position_light_screen	= m_camera->WorldToScreenPoint(position_light_world);
				auto distance				= (position_camera_world - position_light_world).Length() + M_EPSILON;
				auto scale					= GIZMO_MAX_SIZE / distance;
				scale						= Clamp(scale, GIZMO_MIN_SIZE, GIZMO_MAX_SIZE);

				// Choose texture based on light type
				shared_ptr<RHI_Texture> light_tex = nullptr;
				auto type = entity->GetComponent<Light>()->GetLightType();
				if (type == LightType_Directional)	light_tex = m_gizmo_tex_light_directional;
				else if (type == LightType_Point)	light_tex = m_gizmo_tex_light_point;
				else if (type == LightType_Spot)	light_tex = m_gizmo_tex_light_spot;

				// Construct appropriate rectangle
				auto tex_width = light_tex->GetWidth() * scale;
				auto tex_height = light_tex->GetHeight() * scale;
				auto rectangle = Rectangle(position_light_screen.x - tex_width * 0.5f, position_light_screen.y - tex_height * 0.5f, tex_width, tex_height);
				if (rectangle != m_gizmo_light_rect)
				{
					m_gizmo_light_rect = rectangle;
					m_gizmo_light_rect.CreateBuffers(this);
				}

				SetDefaultBuffer(static_cast<uint32_t>(tex_width), static_cast<uint32_t>(tex_width), m_view_projection_orthographic);

				
				m_cmd_list->SetShaderVertex(m_vs_quad);
				m_cmd_list->SetShaderPixel(m_ps_texture);
				m_cmd_list->SetInputLayout(m_vs_quad->GetInputLayout());
				m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
				m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
				m_cmd_list->SetTexture(0, light_tex);
				m_cmd_list->SetBufferIndex(m_gizmo_light_rect.GetIndexBuffer());
				m_cmd_list->SetBufferVertex(m_gizmo_light_rect.GetVertexBuffer());
				m_cmd_list->DrawIndexed(m_gizmo_light_rect.GetIndexCount(), 0, 0);			
				m_cmd_list->Submit();
			}
			m_cmd_list->End();
		}

		// Transform
		if (render_transform && m_gizmo_transform->Update(m_camera.get(), m_gizmo_transform_size, m_gizmo_transform_speed))
		{
			m_cmd_list->Begin("Pass_Gizmos_Transform");

			SetDefaultBuffer(static_cast<uint32_t>(m_resolution.x), static_cast<uint32_t>(m_resolution.y), m_view_projection_orthographic);

			m_cmd_list->SetShaderVertex(m_vps_gizmo_transform);
			m_cmd_list->SetShaderPixel(m_vps_gizmo_transform);
			m_cmd_list->SetInputLayout(m_vps_gizmo_transform->GetInputLayout());
			m_cmd_list->SetBufferIndex(m_gizmo_transform->GetIndexBuffer());
			m_cmd_list->SetBufferVertex(m_gizmo_transform->GetVertexBuffer());
			m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);

			// Axis - X
			auto buffer = Struct_Matrix_Vector3(m_gizmo_transform->GetHandle().GetTransform(Vector3::Right), m_gizmo_transform->GetHandle().GetColor(Vector3::Right));
			m_vps_gizmo_transform->UpdateBuffer(&buffer, 0);
			m_cmd_list->SetConstantBuffer(1, Buffer_Global, m_vps_gizmo_transform->GetConstantBuffer(0));
			m_cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);

			// Axis - Y
			buffer = Struct_Matrix_Vector3(m_gizmo_transform->GetHandle().GetTransform(Vector3::Up), m_gizmo_transform->GetHandle().GetColor(Vector3::Up));
			m_vps_gizmo_transform->UpdateBuffer(&buffer, 1);
			m_cmd_list->SetConstantBuffer(1, Buffer_Global, m_vps_gizmo_transform->GetConstantBuffer(1));
			m_cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);

			// Axis - Z
			buffer = Struct_Matrix_Vector3(m_gizmo_transform->GetHandle().GetTransform(Vector3::Forward), m_gizmo_transform->GetHandle().GetColor(Vector3::Forward));
			m_vps_gizmo_transform->UpdateBuffer(&buffer, 2);
			m_cmd_list->SetConstantBuffer(1, Buffer_Global, m_vps_gizmo_transform->GetConstantBuffer(2));
			m_cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);

			// Axes - XYZ
			if (m_gizmo_transform->DrawXYZ())
			{
				buffer = Struct_Matrix_Vector3(m_gizmo_transform->GetHandle().GetTransform(Vector3::One), m_gizmo_transform->GetHandle().GetColor(Vector3::One));
				m_vps_gizmo_transform->UpdateBuffer(&buffer, 3);
				m_cmd_list->SetConstantBuffer(1, Buffer_Global, m_vps_gizmo_transform->GetConstantBuffer(3));
				m_cmd_list->DrawIndexed(m_gizmo_transform->GetIndexCount(), 0, 0);
			}

			m_cmd_list->End();
		}

		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	void Renderer::Pass_PerformanceMetrics(shared_ptr<RHI_Texture>& tex_out)
	{
		const bool draw = m_flags & Render_Gizmo_PerformanceMetrics;
		if (!draw)
			return;

		m_cmd_list->Begin("Pass_PerformanceMetrics");

		// Updated text
		const auto text_pos = Vector2(-static_cast<int>(m_viewport.GetWidth()) * 0.5f + 1.0f, static_cast<int>(m_viewport.GetHeight()) * 0.5f);
		m_font->SetText(m_profiler->GetMetrics(), text_pos);
		auto buffer = Struct_Matrix_Vector4(m_view_projection_orthographic, m_font->GetColor());
		m_vps_font->UpdateBuffer(&buffer);
	
		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetRenderTarget(tex_out);	
		m_cmd_list->SetViewport(tex_out->GetViewport());
		m_cmd_list->SetBlendState(m_blend_enabled);	
		m_cmd_list->SetTexture(0, m_font->GetAtlas());
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_vps_font->GetConstantBuffer());
		m_cmd_list->SetShaderVertex(m_vps_font);
		m_cmd_list->SetShaderPixel(m_vps_font);
		m_cmd_list->SetInputLayout(m_vps_font->GetInputLayout());	
		m_cmd_list->SetBufferIndex(m_font->GetIndexBuffer());
		m_cmd_list->SetBufferVertex(m_font->GetVertexBuffer());
		m_cmd_list->DrawIndexed(m_font->GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();
	}

	bool Renderer::Pass_DebugBuffer(shared_ptr<RHI_Texture>& tex_out)
	{
		if (m_debug_buffer == RendererDebug_None)
			return true;

		m_cmd_list->Begin("Pass_DebugBuffer");

		SetDefaultBuffer(tex_out->GetWidth(), tex_out->GetHeight(), m_view_projection_orthographic);

		// Bind correct texture & shader pass
		if (m_debug_buffer == RendererDebug_Albedo)
		{
			m_cmd_list->SetTexture(0, m_g_buffer_albedo);
			m_cmd_list->SetShaderPixel(m_ps_texture);
		}

		if (m_debug_buffer == RendererDebug_Normal)
		{
			m_cmd_list->SetTexture(0, m_g_buffer_normal);
			m_cmd_list->SetShaderPixel(m_ps_debug_normal_);
		}

		if (m_debug_buffer == RendererDebug_Material)
		{
			m_cmd_list->SetTexture(0, m_g_buffer_material);
			m_cmd_list->SetShaderPixel(m_ps_texture);
		}

		if (m_debug_buffer == RendererDebug_Velocity)
		{
			m_cmd_list->SetTexture(0, m_g_buffer_velocity);
			m_cmd_list->SetShaderPixel(m_ps_debug_velocity);
		}

		if (m_debug_buffer == RendererDebug_Depth)
		{
			m_cmd_list->SetTexture(0, m_g_buffer_depth);
			m_cmd_list->SetShaderPixel(m_ps_debug_depth);
		}

		if ((m_debug_buffer == RendererDebug_SSAO))
		{
			if (Flags_IsSet(Render_PostProcess_SSAO))
			{
				m_cmd_list->SetTexture(0, m_render_tex_half_ssao);
			}
			else
			{
				m_cmd_list->SetTexture(0, m_tex_white);
			}
			m_cmd_list->SetShaderPixel(m_ps_debug_ssao);
		}

		m_cmd_list->SetDepthStencilState(m_depth_stencil_disabled);
		m_cmd_list->SetRasterizerState(m_rasterizer_cull_back_solid);
		m_cmd_list->SetBlendState(m_blend_disabled);
		m_cmd_list->SetPrimitiveTopology(PrimitiveTopology_TriangleList);
		m_cmd_list->SetRenderTarget(tex_out);
		m_cmd_list->SetViewport(tex_out->GetViewport());	
		m_cmd_list->SetShaderVertex(m_vs_quad);
		m_cmd_list->SetInputLayout(m_vs_quad->GetInputLayout());
		m_cmd_list->SetSampler(0, m_sampler_bilinear_clamp);
		m_cmd_list->SetConstantBuffer(0, Buffer_Global, m_buffer_global);
		m_cmd_list->SetBufferVertex(m_quad.GetVertexBuffer());
		m_cmd_list->SetBufferIndex(m_quad.GetIndexBuffer());
		m_cmd_list->DrawIndexed(m_quad.GetIndexCount(), 0, 0);
		m_cmd_list->End();
		m_cmd_list->Submit();

		return true;
	}
}