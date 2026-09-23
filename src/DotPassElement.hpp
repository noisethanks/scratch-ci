#pragma once

#include <render/pass/PassElement.hpp>

// Stage-2 validation: dot tracks cursor position via Pointer::mgr()->position().
// Proves plugin reads cursor position independently of core's SHADER_POINTER set
// and passes it through its own uniform (proj matrix from projectBoxToTarget).
class CDotPassElement : public IPassElement {
  public:
    CDotPassElement();
    ~CDotPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;

    bool needsLiveBlur() override      { return false; }
    bool needsPrecomputeBlur() override { return false; }

    std::optional<CBox> boundingBox() override;

    const char*      passName() override { return "CDotPassElement"; }
    ePassElementType type() override     { return EK_CUSTOM; }

  private:
    CBox m_box;
};

// Call from PLUGIN_EXIT while the GL context is still active.
void dotPassCleanup();
