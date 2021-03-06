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

#pragma once

//= INCLUDES ==============
#include "RHI_Object.h"
#include "RHI_Definition.h"
//=========================

namespace Spartan
{
	class SPARTAN_CLASS RHI_BlendState : public RHI_Object
	{
	public:
		RHI_BlendState(const std::shared_ptr<RHI_Device>& device,
			bool blend_enabled					= false,
			RHI_Blend source_blend				= Blend_Src_Alpha,
			RHI_Blend dest_blend				= Blend_Inv_Src_Alpha,
			RHI_Blend_Operation blend_op		= Blend_Operation_Add,
			RHI_Blend source_blend_alpha		= Blend_One,
			RHI_Blend dest_blend_alpha			= Blend_One,
			RHI_Blend_Operation blend_op_alpha	= Blend_Operation_Add
		);
		~RHI_BlendState();

		auto GetBlendEnabled()		const { return m_blend_enabled; }
		auto GetSourceBlend()		const { return m_source_blend; }
		auto GetDestBlend()			const { return m_dest_blend; }
		auto GetBlendOp()			const { return m_blend_op; }
		auto GetSourceBlendAlpha()	const { return m_source_blend_alpha; }
		auto GetDestBlendAlpha()	const { return m_dest_blend_alpha; }
		auto GetBlendOpAlpha()		const { return m_blend_op_alpha; }
		auto GetBuffer()			const { return m_buffer; }

		bool operator==(const RHI_BlendState& rhs) const
		{
			return
				m_blend_enabled == rhs.GetBlendEnabled() &&
				m_source_blend == rhs.GetSourceBlend() &&
				m_dest_blend == rhs.GetDestBlend() &&
				m_blend_op == rhs.GetBlendOp() &&
				m_source_blend_alpha == rhs.GetSourceBlendAlpha() &&
				m_dest_blend_alpha == rhs.GetDestBlendAlpha() &&
				m_blend_op_alpha == rhs.GetBlendOpAlpha();
		}

	private:
		bool m_blend_enabled					= false;
		RHI_Blend m_source_blend				= Blend_Src_Alpha;
		RHI_Blend m_dest_blend					= Blend_Inv_Src_Alpha;
		RHI_Blend_Operation m_blend_op			= Blend_Operation_Add;
		RHI_Blend m_source_blend_alpha			= Blend_One;
		RHI_Blend m_dest_blend_alpha			= Blend_One;
		RHI_Blend_Operation m_blend_op_alpha	= Blend_Operation_Add;
	
		void* m_buffer		= nullptr;
		bool m_initialized	= false;
	};
}