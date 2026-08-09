package app.gamenative

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GameHostLifecycleTest {
    @Test
    fun `library activity cannot suspend an active VR game host`() {
        assertFalse(
            GameHostLifecycle.shouldMainActivityControlGameHost(
                isVrSessionActive = true,
            ),
        )
    }

    @Test
    fun `library activity controls an ordinary game host`() {
        assertTrue(
            GameHostLifecycle.shouldMainActivityControlGameHost(
                isVrSessionActive = false,
            ),
        )
    }

    @Test
    fun `resumed host is not suspended after environment setup`() {
        assertFalse(
            GameHostLifecycle.shouldSuspendAfterSetup(
                isHostActivityResumed = true,
                isNeverSuspendMode = false,
            ),
        )
    }

    @Test
    fun `background host is suspended after environment setup`() {
        assertTrue(
            GameHostLifecycle.shouldSuspendAfterSetup(
                isHostActivityResumed = false,
                isNeverSuspendMode = false,
            ),
        )
    }

    @Test
    fun `never suspend policy wins while host is backgrounded`() {
        assertFalse(
            GameHostLifecycle.shouldSuspendAfterSetup(
                isHostActivityResumed = false,
                isNeverSuspendMode = true,
            ),
        )
    }

    @Test
    fun `ordinary activity destruction tears down the game host`() {
        assertTrue(
            GameHostLifecycle.shouldTearDownEnvironment(
                isChangingConfigurations = false,
                isVrSessionActive = false,
            ),
        )
    }

    @Test
    fun `background library destruction preserves an active VR game`() {
        assertFalse(
            GameHostLifecycle.shouldTearDownEnvironment(
                isChangingConfigurations = false,
                isVrSessionActive = true,
            ),
        )
    }

    @Test
    fun `configuration changes preserve the game host`() {
        assertFalse(
            GameHostLifecycle.shouldTearDownEnvironment(
                isChangingConfigurations = true,
                isVrSessionActive = false,
            ),
        )
    }
}
