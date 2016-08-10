/*
Copyright(c) 2016 Panos Karabelas

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

//= INCLUDES ====================
#include "DirectusIconProvider.h"
#include "IO/Log.h"
#include "IO/FileHelper.h"
#include "Loading/ImageLoader.h"
//===============================

//= NAMESPACES =====
using namespace std;
//==================

void DirectusIconProvider::Initialize()
{
    m_imageLoader = new ImageLoader();

    m_unknownIcon = QIcon(":/Images/file.png");
    m_folderIcon = QIcon(":/Images/folder.png");
    m_imageIcon = QIcon(":/Images/image.png");
    m_modelIcon = QIcon(":/Images/model.png");
    m_scriptIcon = QIcon(":/Images/scriptLarge.png");
    m_sceneIcon = QIcon(":/Images/scene.png");
    m_shaderIcon = QIcon(":/Images/hlsl.png");
}

// Returns an icon for the file described by info.
QIcon DirectusIconProvider::icon(const QFileInfo& info) const
{
    // Folder
    if (info.isDir())
        return m_folderIcon;

    string filePath = info.absoluteFilePath().toStdString();

    // Icon
    if(FileHelper::IsSupportedImage(filePath))
    {
        int width = 100;
        int heigh = 100;
        QPixmap pixmap;
        m_imageLoader->Load(filePath, width, heigh);

        QImage image;
        image =  QImage(
                    (const uchar*)m_imageLoader->GetRGBA(),
                    width,
                    heigh,
                    QImage::Format_RGBA8888
                    );

        pixmap = QPixmap::fromImage(image);

        m_imageLoader->Clear();
        return pixmap;
    }

    // Model
    if (FileHelper::IsSupportedModel(filePath))
    {
        return m_modelIcon;
    }

    // Script
    if (FileHelper::IsSupportedScript(filePath))
    {
        return m_scriptIcon;
    }

    // Scene
    if (FileHelper::IsSupportedScene(filePath))
    {
        return m_sceneIcon;
    }

    // Shader
    if (FileHelper::IsSupportedShader(filePath))
    {
        return m_shaderIcon;
    }

    // Unknown
    return m_unknownIcon;
}