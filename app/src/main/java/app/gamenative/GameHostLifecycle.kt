package app.gamenative

internal object GameHostLifecycle {
    fun shouldMainActivityControlGameHost(isVrSessionActive: Boolean): Boolean =
        !isVrSessionActive

    fun shouldSuspendAfterSetup(
        isHostActivityResumed: Boolean,
        isNeverSuspendMode: Boolean,
    ): Boolean = !isHostActivityResumed && !isNeverSuspendMode

    fun shouldTearDownEnvironment(
        isChangingConfigurations: Boolean,
        isVrSessionActive: Boolean,
    ): Boolean = !isChangingConfigurations && !isVrSessionActive
}
