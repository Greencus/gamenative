package app.gamenative.xr;

import android.annotation.SuppressLint;
import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;

import com.winlator.renderer.VulkanRenderer;
import com.winlator.widget.XServerRendererView;
import com.winlator.xserver.XServer;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

@SuppressLint("ViewConstructor")
public class XrXServerView extends View implements XServerRendererView, QuestVrSurfaceRegistry.Listener {
    private final VulkanRenderer renderer;
    private final XServer xServer;
    private final ExecutorService eventExecutor = Executors.newSingleThreadExecutor();
    private int frameRateLimit = 0;
    private boolean hasSurface = false;

    public XrXServerView(Context context, XServer xServer) {
        super(context);
        setLayoutParams(new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setAlpha(0.0f);
        this.xServer = xServer;
        renderer = new VulkanRenderer(this, xServer);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        QuestVrSurfaceRegistry.INSTANCE.addListener(this);
    }

    @Override
    protected void onDetachedFromWindow() {
        QuestVrSurfaceRegistry.INSTANCE.removeListener(this);
        if (hasSurface) {
            renderer.onSurfaceDestroyed();
            hasSurface = false;
        }
        super.onDetachedFromWindow();
    }

    @Override
    public void onXrSurfaceReady(QuestVrSurfaceRegistry.Target target) {
        post(() -> {
            if (hasSurface) {
                renderer.onSurfaceDestroyed();
            }
            renderer.onSurfaceCreated(target.getSurface());
            renderer.onSurfaceChanged(target.getWidth(), target.getHeight());
            hasSurface = true;
        });
    }

    @Override
    public void onXrSurfaceDestroyed() {
        post(() -> {
            if (hasSurface) {
                renderer.onSurfaceDestroyed();
                hasSurface = false;
            }
        });
    }

    public XServer getxServer() {
        return xServer;
    }

    public VulkanRenderer getRenderer() {
        return renderer;
    }

    public void queueEvent(Runnable r) {
        eventExecutor.execute(r);
    }

    public void onPause() {}
    public void onResume() {}

    public int getFrameRateLimit() {
        return frameRateLimit;
    }

    public void setFrameRateLimit(int frameRateLimit) {
        this.frameRateLimit = Math.max(0, frameRateLimit);
        renderer.setFpsLimit(this.frameRateLimit);
    }

    public void requestRender() {
        renderer.queueSceneUpdate();
    }
}
