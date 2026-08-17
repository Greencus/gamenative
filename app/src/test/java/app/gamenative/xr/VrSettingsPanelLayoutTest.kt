package app.gamenative.xr

import org.junit.Assert.assertEquals
import org.junit.Test

class VrSettingsPanelLayoutTest {
    @Test
    fun `right-controller UV coordinates select every visible control`() {
        assertEquals(
            VrSettingsPanelLayout.Control.RENDER_SCALE_DOWN,
            VrSettingsPanelLayout.hitTest(665f / 1024f, 295f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.RENDER_SCALE_UP,
            VrSettingsPanelLayout.hitTest(905f / 1024f, 295f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.PACING_NATIVE,
            VrSettingsPanelLayout.hitTest(650f / 1024f, 480f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.PACING_HALF,
            VrSettingsPanelLayout.hitTest(850f / 1024f, 480f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.OPENCOMPOSITE,
            VrSettingsPanelLayout.hitTest(500f / 1024f, 660f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.THEATER,
            VrSettingsPanelLayout.hitTest(500f / 1024f, 830f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.CLOCK,
            VrSettingsPanelLayout.hitTest(500f / 1024f, 1000f / 1280f),
        )
        assertEquals(
            VrSettingsPanelLayout.Control.CLOSE,
            VrSettingsPanelLayout.hitTest(500f / 1024f, 1160f / 1280f),
        )
    }

    @Test
    fun `empty panel space does not activate a setting`() {
        assertEquals(VrSettingsPanelLayout.Control.NONE, VrSettingsPanelLayout.hitTest(0.5f, 0.1f))
        assertEquals(VrSettingsPanelLayout.Control.NONE, VrSettingsPanelLayout.hitTest(0.03f, 0.5f))
        assertEquals(VrSettingsPanelLayout.Control.NONE, VrSettingsPanelLayout.hitTest(0.5f, 0.85f))
    }
}
