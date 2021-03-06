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

//= INCLUDES ==================
#include <memory>
#include "RHI_Object.h"
#include "RHI_Definition.h"
#include "../Core/EngineDefs.h"
//=============================

namespace Spartan
{
	class SPARTAN_CLASS RHI_ConstantBuffer : public RHI_Object
	{
	public:
		RHI_ConstantBuffer(const std::shared_ptr<RHI_Device>& rhi_device)
		{
			m_rhi_device = rhi_device;
		}
		~RHI_ConstantBuffer();

		template<typename T>
		bool Create()
		{
			m_size = static_cast<uint32_t>(sizeof(T));
			return _Create();
		}

		void* Map() const;
		bool Unmap() const;
		auto GetResource() const	{ return m_buffer; }
		auto GetSize()	const		{ return m_size; }

	private:
		bool _Create();

		std::shared_ptr<RHI_Device> m_rhi_device;

		// API
		void* m_buffer			= nullptr;
		void* m_buffer_memory	= nullptr;
	};
}