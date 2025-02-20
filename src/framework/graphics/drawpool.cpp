/*
 * Copyright (c) 2010-2020 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "drawpool.h"
#include "declarations.h"
#include <framework/core/declarations.h>
#include <framework/graphics/framebuffermanager.h>
#include <framework/graphics/graphics.h>
#include "painter.h"

const static std::hash<size_t> HASH_INT;
const static std::hash<float> HASH_FLOAT;

DrawPool g_drawPool;

void DrawPool::init()
{
    n_unknowPool = g_drawPool.createPool(PoolType::UNKNOW);
    use(n_unknowPool);
}

void DrawPool::terminate()
{
    m_currentPool = nullptr;
    for(int8 i = -1; ++i <= PoolType::UNKNOW;)
        m_pools[i] = nullptr;
}

PoolFramedPtr DrawPool::createPoolF(const PoolType type)
{
    auto pool = std::make_shared<FramedPool>();

    pool->m_framebuffer = g_framebuffers.createFrameBuffer(true);

    if(type == PoolType::MAP) pool->m_framebuffer->disableBlend();
    else if(type == PoolType::LIGHT) pool->m_framebuffer->setCompositionMode(Painter::CompositionMode_Light);

    m_pools[type] = pool;

    return pool;
}

void DrawPool::addRepeated(const Painter::PainterState& state, const Pool::DrawMethod& method, const Painter::DrawMode drawMode)
{
    updateHash(state, method);

    const uint16 startIndex = m_currentPool->m_indexToStartSearching ? m_currentPool->m_indexToStartSearching - 1 : 0;

    const auto itFind = std::find_if(m_currentPool->m_objects.begin() + startIndex, m_currentPool->m_objects.end(), [state]
    (const Pool::DrawObject& action) { return action.state == state; });

    if(itFind != m_currentPool->m_objects.end()) {
        (*itFind).drawMethods.push_back(method);
    } else
        m_currentPool->m_objects.push_back(Pool::DrawObject{ state, drawMode, {method} });
}

void DrawPool::add(const Painter::PainterState& state, const Pool::DrawMethod& method, const Painter::DrawMode drawMode)
{
    updateHash(state, method);

    auto& list = m_currentPool->m_objects;

    if(!list.empty()) {
        auto& prevObj = list.back();

        const bool sameState = prevObj.state == state;
        if(!method.dest.isNull()) {
            // Look for identical or opaque textures that are greater than or
            // equal to the size of the previous texture, if so, remove it from the list so they don't get drawn.
            for(auto itm = prevObj.drawMethods.begin(); itm != prevObj.drawMethods.end(); ++itm) {
                auto& prevMtd = *itm;
                if(prevMtd.dest == method.dest &&
                   ((sameState && prevMtd.rects.second == method.rects.second) || (state.texture->isOpaque() && prevObj.state.texture->canSuperimposed()))) {
                    prevObj.drawMethods.erase(itm);
                    break;
                }
            }
        }

        if(sameState) {
            prevObj.drawMode = Painter::DrawMode::Triangles;
            prevObj.drawMethods.push_back(method);
            return;
        }
    }

    list.push_back(Pool::DrawObject{ state, drawMode, {method} });
}

void DrawPool::draw()
{
    // Pre Draw
    for(const auto& pool : m_pools) {
        if(!pool->isEnabled() || !pool->hasFrameBuffer()) continue;
        const auto& pf = pool->toFramedPool();
        if(pf->hasModification()) {
            pf->updateStatus();
            if(!pool->m_objects.empty()) {
                pf->m_framebuffer->bind();
                for(auto& obj : pool->m_objects)
                    drawObject(obj);
                pf->m_framebuffer->release();
            }
        }
    }

    // Draw
    for(const auto& pool : m_pools) {
        if(!pool->isEnabled()) continue;
        if(pool->hasFrameBuffer()) {
            const auto pf = pool->toFramedPool();

            g_painter->saveAndResetState();
            if(pf->m_beforeDraw) pf->m_beforeDraw();
            pf->m_framebuffer->draw(pf->m_dest, pf->m_src);
            if(pf->m_afterDraw) pf->m_afterDraw();
            g_painter->restoreSavedState();
        } else             for(auto& obj : pool->m_objects)
            drawObject(obj);

        pool->m_objects.clear();
    }
}

void DrawPool::drawObject(Pool::DrawObject& obj)
{
    if(obj.action) {
        obj.action();
        return;
    }

    if(obj.drawMethods.empty()) return;

    g_painter->executeState(obj.state);

    if(obj.state.texture) {
        obj.state.texture->create();
        g_painter->setTexture(obj.state.texture.get());
    }

    for(const auto& method : obj.drawMethods) {
        if(method.type == Pool::DrawMethodType::DRAW_BOUNDING_RECT) {
            m_coordsbuffer.addBoudingRect(method.rects.first, method.intValue);
        } else if(method.type == Pool::DrawMethodType::DRAW_FILLED_RECT || method.type == Pool::DrawMethodType::DRAW_REPEATED_FILLED_RECT) {
            m_coordsbuffer.addRect(method.rects.first);
        } else if(method.type == Pool::DrawMethodType::DRAW_FILLED_TRIANGLE) {
            m_coordsbuffer.addTriangle(std::get<0>(method.points), std::get<1>(method.points), std::get<2>(method.points));
        } else if(method.type == Pool::DrawMethodType::DRAW_TEXTURED_RECT || method.type == Pool::DrawMethodType::DRAW_REPEATED_TEXTURED_RECT) {
            if(obj.drawMode == Painter::DrawMode::Triangles)
                m_coordsbuffer.addRect(method.rects.first, method.rects.second);
            else
                m_coordsbuffer.addQuad(method.rects.first, method.rects.second);
        } else if(method.type == Pool::DrawMethodType::DRAW_UPSIDEDOWN_TEXTURED_RECT) {
            if(obj.drawMode == Painter::DrawMode::Triangles)
                m_coordsbuffer.addUpsideDownRect(method.rects.first, method.rects.second);
            else
                m_coordsbuffer.addUpsideDownQuad(method.rects.first, method.rects.second);
        } else if(method.type == Pool::DrawMethodType::DRAW_REPEATED_TEXTURED_REPEATED_RECT) {
            m_coordsbuffer.addRepeatedRects(method.rects.first, method.rects.second);
        }
    }

    g_painter->drawCoords(m_coordsbuffer, obj.drawMode);
    m_coordsbuffer.clear();
}

void DrawPool::addTexturedRect(const Rect& dest, const TexturePtr& texture, const Color color)
{
    addTexturedRect(dest, texture, Rect(Point(), texture->getSize()), color);
}

void DrawPool::addTexturedRect(const Rect& dest, const TexturePtr& texture, const Rect& src, const Color color, const Point& originalDest)
{
    if(dest.isEmpty() || src.isEmpty())
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_TEXTURED_RECT };
    method.rects = std::make_pair(dest, src);
    method.dest = originalDest;

    auto state = generateState();
    state.color = color;
    state.texture = texture;

    add(state, method, Painter::DrawMode::TriangleStrip);
}

void DrawPool::addUpsideDownTexturedRect(const Rect& dest, const TexturePtr& texture, const Rect& src, const Color color)
{
    if(dest.isEmpty() || src.isEmpty())
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_UPSIDEDOWN_TEXTURED_RECT };
    method.rects = std::make_pair(dest, src);

    auto state = generateState();
    state.color = color;
    state.texture = texture;

    add(state, method, Painter::DrawMode::TriangleStrip);
}

void DrawPool::addRepeatedTexturedRect(const Rect& dest, const TexturePtr& texture, const Color color)
{
    addRepeatedTexturedRect(dest, texture, Rect(Point(), texture->getSize()), color);
}

void DrawPool::addRepeatedTexturedRect(const Rect& dest, const TexturePtr& texture, const Rect& src, const Color color)
{
    if(dest.isEmpty() || src.isEmpty())
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_REPEATED_TEXTURED_RECT };
    method.rects = std::make_pair(dest, src);

    auto state = generateState();
    state.color = color;
    state.texture = texture;

    addRepeated(state, method);
}

void DrawPool::addRepeatedTexturedRepeatedRect(const Rect& dest, const TexturePtr& texture, const Rect& src, const Color color)
{
    if(dest.isEmpty() || src.isEmpty())
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_REPEATED_TEXTURED_REPEATED_RECT };
    method.rects = std::make_pair(dest, src);

    auto state = generateState();
    state.color = color;
    state.texture = texture;

    addRepeated(state, method);
}

void DrawPool::addRepeatedFilledRect(const Rect& dest, const Color color)
{
    addRepeatedFilledRect(dest, Rect(), color);
}

void DrawPool::addRepeatedFilledRect(const Rect& dest, const Rect& src, const Color color)
{
    if(dest.isEmpty())
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_REPEATED_FILLED_RECT };
    method.rects = std::make_pair(dest, src);

    auto state = generateState();
    state.color = color;

    addRepeated(state, method);
}

void DrawPool::addFilledRect(const Rect& dest, const Color color)
{
    addFilledRect(dest, Rect(), color);
}

void DrawPool::addFilledRect(const Rect& dest, const Rect& src, const Color color)
{
    if(dest.isEmpty())
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_FILLED_RECT };
    method.rects = std::make_pair(dest, src);

    auto state = generateState();
    state.color = color;

    add(state, method);
}

void DrawPool::addFilledTriangle(const Point& a, const Point& b, const Point& c, const Color color)
{
    if(a == b || a == c || b == c)
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_FILLED_TRIANGLE };
    method.points = std::make_tuple(a, b, c);

    auto state = generateState();
    state.color = color;

    add(state, method);
}

void DrawPool::addBoundingRect(const Rect& dest, const Color color, int innerLineWidth)
{
    if(dest.isEmpty() || innerLineWidth == 0)
        return;

    Pool::DrawMethod method{ Pool::DrawMethodType::DRAW_BOUNDING_RECT };
    method.rects = std::make_pair(dest, Rect());
    method.intValue = innerLineWidth;

    auto state = generateState();
    state.color = color;

    add(state, method);
}

void DrawPool::addAction(std::function<void()> action)
{
    m_currentPool->m_objects.push_back(Pool::DrawObject{ {}, Painter::DrawMode::None, {}, action });
}

Painter::PainterState DrawPool::generateState()
{
    Painter::PainterState state = g_painter->getCurrentState();
    state.clipRect = m_currentPool->m_state.clipRect;
    state.compositionMode = m_currentPool->m_state.compositionMode;
    state.opacity = m_currentPool->m_state.opacity;
    state.alphaWriting = m_currentPool->m_state.alphaWriting;
    state.shaderProgram = m_currentPool->m_state.shaderProgram;

    return state;
}

void DrawPool::use(const PoolPtr& pool)
{
    m_currentPool = pool ? pool : n_unknowPool;
    m_currentPool->resetState();
    if(m_currentPool->hasFrameBuffer()) {
        poolFramed()->resetCurrentStatus();
    }
}

void DrawPool::use(const PoolFramedPtr& pool, const Rect& dest, const Rect& src)
{
    use(pool);
    pool->m_dest = dest;
    pool->m_src = src;
    pool->m_state.alphaWriting = false;
}

void DrawPool::updateHash(const Painter::PainterState& state, const Pool::DrawMethod& method)
{
    if(!m_currentPool->hasFrameBuffer()) return;

    size_t hash = 0;

    if(state.texture) {
        // TODO: use uniqueID id when applying multithreading, not forgetting that in the APNG texture, the id changes every frame.
        boost::hash_combine(hash, HASH_INT(state.texture->getId()));
    }

    if(state.opacity < 1.f)
        boost::hash_combine(hash, HASH_FLOAT(state.opacity));

    if(state.color != Color::white)
        boost::hash_combine(hash, HASH_INT(state.color.rgba()));

    if(state.compositionMode != Painter::CompositionMode_Normal)
        boost::hash_combine(hash, HASH_INT(state.compositionMode));

    if(state.shaderProgram)
        poolFramed()->m_autoUpdate = true;

    if(state.clipRect.isValid()) boost::hash_combine(hash, state.clipRect.hash());
    if(method.rects.first.isValid()) boost::hash_combine(hash, method.rects.first.hash());
    if(method.rects.second.isValid()) boost::hash_combine(hash, method.rects.second.hash());

    const auto& a = std::get<0>(method.points),
        b = std::get<1>(method.points),
        c = std::get<2>(method.points);

    if(!a.isNull()) boost::hash_combine(hash, a.hash());
    if(!b.isNull()) boost::hash_combine(hash, b.hash());
    if(!c.isNull()) boost::hash_combine(hash, c.hash());

    if(method.intValue) boost::hash_combine(hash, HASH_INT(method.intValue));
    if(method.hash) boost::hash_combine(hash, method.hash);

    boost::hash_combine(poolFramed()->m_status.second, hash);
}
