#pragma once

#include "TrailBuffer.hpp"

// Per-app suppression state (SPEC section 7): whether the pointer is over a
// window whose hyprtail:no_trail rule is set, and what its changes do to the
// trail's point buffer. The window lookup and the damage are the caller's;
// this is only the edge detection and the buffer reset, so it needs no
// Hyprland headers. Header-only for the unit tests.
namespace hyprtail {
    enum class eGateEdge {
        NONE,
        ENTER, // the pointer moved onto an excluded window
        EXIT,  // ... off every excluded window
    };

    class CPointerGate {
      public:
        // Whether the pointer is over an excluded window now. On a change the
        // source is cleared: on ENTER nothing may stay to draw or to join the
        // next node, on EXIT the first new node must not connect to one from
        // before the excluded window. Returns the change, NONE if there was
        // none. The source is not touched otherwise.
        eGateEdge update(bool excluded, ISource& source) {
            if (excluded == m_excluded)
                return eGateEdge::NONE;
            m_excluded = excluded;
            source.clear();
            return excluded ? eGateEdge::ENTER : eGateEdge::EXIT;
        }

        bool excluded() const {
            return m_excluded;
        }

      private:
        bool m_excluded = false;
    };
}
