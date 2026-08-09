package app.gamenative.ui.component.dialog

import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import app.gamenative.R
import app.gamenative.ui.component.settings.SettingsListDropdown
import app.gamenative.ui.theme.settingsTileColors
import app.gamenative.ui.theme.settingsTileColorsAlt
import app.gamenative.xr.XrContainerSettings
import com.alorma.compose.settings.ui.SettingsGroup
import com.alorma.compose.settings.ui.SettingsSwitch
import com.winlator.container.ContainerData

@Composable
fun VrTabContent(
    state: ContainerConfigState,
    onConfigChanged: (ContainerData) -> Unit = {},
) {
    val config = state.config.value
    val renderScales = XrContainerSettings.renderScaleOptions
    val framePacingDivisors = XrContainerSettings.framePacingDivisorOptions

    SettingsGroup {
        SettingsListDropdown(
            colors = settingsTileColors(),
            title = { Text(text = stringResource(R.string.vr_render_resolution)) },
            subtitle = { Text(text = stringResource(R.string.vr_render_resolution_description)) },
            value = renderScales.indexOf(config.xrRenderScale).coerceAtLeast(0),
            items = renderScales.map { "$it%" },
            onItemSelected = { index ->
                val updated = state.config.value.copy(xrRenderScale = renderScales[index])
                state.config.value = updated
                onConfigChanged(updated)
            },
        )
        SettingsListDropdown(
            colors = settingsTileColorsAlt(),
            title = { Text(text = stringResource(R.string.vr_frame_pacing)) },
            subtitle = { Text(text = stringResource(R.string.vr_frame_pacing_description)) },
            value = framePacingDivisors.indexOf(config.xrFramePacingDivisor).coerceAtLeast(0),
            items = listOf(
                stringResource(R.string.vr_frame_pacing_native),
                stringResource(R.string.vr_frame_pacing_half),
            ),
            onItemSelected = { index ->
                val updated = state.config.value.copy(
                    xrFramePacingDivisor = framePacingDivisors[index],
                )
                state.config.value = updated
                onConfigChanged(updated)
            },
        )
        SettingsSwitch(
            colors = settingsTileColorsAlt(),
            title = { Text(text = stringResource(R.string.vr_opencomposite)) },
            subtitle = { Text(text = stringResource(R.string.vr_opencomposite_description)) },
            state = config.xrOpenCompositeEnabled,
            onCheckedChange = {
                val updated = state.config.value.copy(xrOpenCompositeEnabled = it)
                state.config.value = updated
                onConfigChanged(updated)
            },
        )
        SettingsSwitch(
            colors = settingsTileColorsAlt(),
            title = { Text(text = stringResource(R.string.vr_theater_screen)) },
            subtitle = { Text(text = stringResource(R.string.vr_theater_screen_description)) },
            state = config.xrTheaterScreenEnabled,
            onCheckedChange = {
                val updated = state.config.value.copy(xrTheaterScreenEnabled = it)
                state.config.value = updated
                onConfigChanged(updated)
            },
        )
        SettingsSwitch(
            colors = settingsTileColorsAlt(),
            title = { Text(text = stringResource(R.string.vr_left_hand_clock)) },
            subtitle = { Text(text = stringResource(R.string.vr_left_hand_clock_description)) },
            state = config.xrClockEnabled,
            onCheckedChange = {
                val updated = state.config.value.copy(xrClockEnabled = it)
                state.config.value = updated
                onConfigChanged(updated)
            },
        )
    }
}
