
/* Copyright (c) 2011-2017, Stefan Eilemann <eile@eyescale.ch>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Eyescale Software GmbH nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SEQ_PLY_H
#define SEQ_PLY_H

#include <seq/seq.h>

#include <frameData.h>
#include <triply/vertexBufferDist.h>
#include <triply/vertexBufferRoot.h>
#include <triply/vertexBufferState.h>

#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif

/** The Sequel polygonal rendering example. */
namespace seqPly
{
typedef triply::VertexBufferRoot Model;
typedef triply::VertexBufferDist ModelDist;
typedef triply::VertexBufferStateSimple State;
using eqPly::FrameData;

class Application : public seq::Application
{
public:
    Application() {}
    bool init(const int argc, char** argv);
    bool run();
    virtual bool exit();

    seq::Renderer* createRenderer() final;
    co::Object* createObject(const uint32_t type) final;

    const Model* getModel(const eq::uint128_t& modelID);

private:
    FrameData _frameData;
    std::unique_ptr<Model> _model;
    std::unique_ptr<ModelDist> _modelDist;
    std::mutex _modelLock;

    virtual ~Application() {}
    eq::Strings _parseArguments(const int argc, char** argv);
    void _loadModel(const eq::Strings& models);
    void _unloadModel();
};

typedef lunchbox::RefPtr<Application> ApplicationPtr;
}

#endif // SEQ_PLY_H
