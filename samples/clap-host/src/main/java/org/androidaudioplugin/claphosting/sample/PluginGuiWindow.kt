package org.androidaudioplugin.claphosting.sample

import android.view.View
import android.view.ViewGroup
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import kotlin.math.roundToInt

/**
 * A floating window for a plugin UI embedded via CLAP_WINDOW_API_AAP_GUI.
 *
 * Ported from uapmd-kmp's AapPluginSurfaceWindow (AndroidPlatformHostedPluginUiLayer.kt), which
 * follows aaphostsample's PluginSurfaceControlUI: the SurfaceView is sized to the viewport, and the
 * plugin renders the scrolled region of its content into it (AAP_CLAP_EXT_GUI_VIEWPORT).
 *
 * @param preferredContentWidth the plugin UI content size from CLAP gui.get_size() or
 *   clap_host_gui.request_resize(), or 0 if the plugin does not tell it (yet); the available
 *   area is used until then.
 * @param onConfigureViewport configures the viewport, content size and scroll position.
 * @param onShow calls CLAP gui.show(), which connects the plugin UI to the attached SurfaceView.
 */
@Composable
fun PluginGuiWindow(
    title: String,
    surfaceView: View,
    preferredContentWidth: Int,
    preferredContentHeight: Int,
    onConfigureViewport: (viewportWidth: Int, viewportHeight: Int,
                          contentWidth: Int, contentHeight: Int,
                          scrollX: Int, scrollY: Int) -> Unit,
    onShow: () -> Unit,
    onClose: () -> Unit
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val scrollbarThickness = 12.dp
        val resizeHandleSize = 20.dp
        // Only an estimate of the title bar's own height, used to keep the window
        // within the host area; the bar itself is sized by its content.
        val titleBarHeight = 52.dp
        val scrollbarThicknessPx = with(density) { scrollbarThickness.roundToPx() }
        val chromeHeightPx = with(density) { (titleBarHeight + scrollbarThickness).roundToPx() }
        // The viewport may grow up to the content size, but never past what the host area can show.
        val maxAvailableWidthPx = (constraints.maxWidth - scrollbarThicknessPx).coerceAtLeast(1)
        val maxAvailableHeightPx = (constraints.maxHeight - chromeHeightPx).coerceAtLeast(1)
        val minDimensionPx = with(density) { MIN_VIEWPORT_DIMENSION.roundToPx() }

        // A plugin that does not tell its UI size (e.g. AAP's default Compose UI) gets the host
        // area, so that its window can still be resized.
        var contentWidthPx by remember(surfaceView) {
            mutableStateOf(preferredContentWidth.takeIf { it > 0 }
                ?: constraints.maxWidth.coerceAtLeast(MIN_CONTENT_FALLBACK_WIDTH))
        }
        var contentHeightPx by remember(surfaceView) {
            mutableStateOf(preferredContentHeight.takeIf { it > 0 }
                ?: constraints.maxHeight.coerceAtLeast(MIN_CONTENT_FALLBACK_HEIGHT))
        }
        // Open at 80% of the host area at most, so the window lands fully on screen.
        var viewportWidthPx by remember(surfaceView) {
            mutableStateOf(constrainViewport(contentWidthPx,
                (maxAvailableWidthPx * INITIAL_MAX_HOST_FRACTION).toInt(), contentWidthPx, minDimensionPx))
        }
        var viewportHeightPx by remember(surfaceView) {
            mutableStateOf(constrainViewport(contentHeightPx,
                (maxAvailableHeightPx * INITIAL_MAX_HOST_FRACTION).toInt(), contentHeightPx, minDimensionPx))
        }
        var offsetX by remember(surfaceView) { mutableStateOf(0f) }
        var offsetY by remember(surfaceView) { mutableStateOf(0f) }
        var scrollX by remember(surfaceView) { mutableStateOf(0) }
        var scrollY by remember(surfaceView) { mutableStateOf(0) }
        var surfaceReady by remember(surfaceView) { mutableStateOf(false) }
        var shown by remember(surfaceView) { mutableStateOf(false) }
        var resizeStartWidth by remember { mutableStateOf(viewportWidthPx) }
        var resizeStartHeight by remember { mutableStateOf(viewportHeightPx) }
        var resizeDragWidth by remember { mutableStateOf(0f) }
        var resizeDragHeight by remember { mutableStateOf(0f) }
        val currentOnConfigureViewport by rememberUpdatedState(onConfigureViewport)
        val currentOnShow by rememberUpdatedState(onShow)

        val viewportWidthDp = with(density) { viewportWidthPx.toDp() }
        val viewportHeightDp = with(density) { viewportHeightPx.toDp() }
        val maxViewportWidthPx = minOf(contentWidthPx, maxAvailableWidthPx).coerceAtLeast(1)
        val maxViewportHeightPx = minOf(contentHeightPx, maxAvailableHeightPx).coerceAtLeast(1)
        val minViewportWidthPx = minDimensionPx.coerceAtMost(maxViewportWidthPx)
        val minViewportHeightPx = minDimensionPx.coerceAtMost(maxViewportHeightPx)
        val maxScrollX = (contentWidthPx - viewportWidthPx).coerceAtLeast(0)
        val maxScrollY = (contentHeightPx - viewportHeightPx).coerceAtLeast(0)
        val effectiveScrollX = scrollX.coerceIn(0, maxScrollX)
        val effectiveScrollY = scrollY.coerceIn(0, maxScrollY)
        val frameWidthDp = viewportWidthDp + if (maxScrollY > 0) scrollbarThickness else 0.dp

        // The plugin may tell its UI size later (clap_host_gui.request_resize); fit the window to it.
        LaunchedEffect(preferredContentWidth, preferredContentHeight) {
            if (preferredContentWidth <= 0 || preferredContentHeight <= 0 ||
                (preferredContentWidth == contentWidthPx && preferredContentHeight == contentHeightPx))
                return@LaunchedEffect
            contentWidthPx = preferredContentWidth
            contentHeightPx = preferredContentHeight
            viewportWidthPx = constrainViewport(contentWidthPx,
                (maxAvailableWidthPx * INITIAL_MAX_HOST_FRACTION).toInt(), contentWidthPx, minDimensionPx)
            viewportHeightPx = constrainViewport(contentHeightPx,
                (maxAvailableHeightPx * INITIAL_MAX_HOST_FRACTION).toInt(), contentHeightPx, minDimensionPx)
        }

        // The SurfaceView has to be in a live view tree with a display and layout params before
        // the plugin UI gets connected (gui.show()); connecting earlier races the
        // SurfaceControlViewHost setup and leaves the plugin surface blank.
        DisposableEffect(surfaceView) {
            var cancelled = false
            val notifyWhenReady = object : Runnable {
                override fun run() {
                    if (cancelled)
                        return
                    if (!surfaceView.isAttachedToWindow || surfaceView.display == null || surfaceView.layoutParams == null) {
                        surfaceView.post(this)
                        return
                    }
                    surfaceReady = true
                }
            }
            surfaceView.post(notifyWhenReady)
            onDispose {
                cancelled = true
                surfaceReady = false
            }
        }

        // Deliberately not keyed on the viewport size, so that it runs gui.show() only once.
        LaunchedEffect(surfaceView, surfaceReady) {
            if (!surfaceReady || shown)
                return@LaunchedEffect
            // The plugin connects at the content size, so it has to know it first.
            currentOnConfigureViewport(viewportWidthPx, viewportHeightPx, contentWidthPx, contentHeightPx,
                effectiveScrollX, effectiveScrollY)
            currentOnShow()
            shown = true
        }

        LaunchedEffect(surfaceView, viewportWidthPx, viewportHeightPx, contentWidthPx, contentHeightPx,
            effectiveScrollX, effectiveScrollY, shown) {
            if (!shown)
                return@LaunchedEffect
            currentOnConfigureViewport(viewportWidthPx, viewportHeightPx, contentWidthPx, contentHeightPx,
                effectiveScrollX, effectiveScrollY)
        }

        // Drawn in the activity window, not in a Popup, so that the plugin UI can take focus
        // (and the IME) through the SurfaceView's host window.
        Box(
            modifier = Modifier
                .wrapContentSize(Alignment.TopStart, unbounded = true)
                .offset { IntOffset(offsetX.roundToInt(), offsetY.roundToInt()) }
        ) {
            Column(
                modifier = Modifier
                    .padding(12.dp)
                    // Presses on the frame must not fall through to the controls underneath.
                    .pointerInput(Unit) {
                        awaitPointerEventScope {
                            while (true)
                                awaitPointerEvent()
                        }
                    }
                    .background(MaterialTheme.colorScheme.surface)
                    .border(1.dp, MaterialTheme.colorScheme.outlineVariant)
            ) {
                Row(
                    modifier = Modifier
                        .width(frameWidthDp)
                        .background(MaterialTheme.colorScheme.primaryContainer)
                        .pointerInput(Unit) {
                            detectDragGestures { change, dragAmount ->
                                change.consume()
                                offsetX += dragAmount.x
                                offsetY += dragAmount.y
                            }
                        }
                        .padding(horizontal = 8.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(title, modifier = Modifier.weight(1f), maxLines = 1,
                        color = MaterialTheme.colorScheme.onPrimaryContainer)
                    TextButton(onClick = onClose) {
                        Text("Close")
                    }
                }
                Row {
                    Box(
                        modifier = Modifier
                            .width(viewportWidthDp)
                            .height(viewportHeightDp)
                            .border(1.dp, MaterialTheme.colorScheme.outline)
                    ) {
                        AndroidView(
                            modifier = Modifier.fillMaxSize(),
                            factory = { _ -> surfaceView },
                            // The plugin keeps the same SurfaceView for the next gui.create().
                            onRelease = { v -> (v.parent as? ViewGroup)?.removeView(v) },
                            onReset = { _ -> }
                        )
                    }
                    if (maxScrollY > 0) {
                        ScrollbarTrack(
                            modifier = Modifier
                                .width(scrollbarThickness)
                                .height(viewportHeightDp),
                            scrollValue = effectiveScrollY,
                            maxValue = maxScrollY,
                            isHorizontal = false,
                            onScrollValueChange = { scrollY = it }
                        )
                    }
                }
                Row {
                    if (maxScrollX > 0) {
                        ScrollbarTrack(
                            modifier = Modifier
                                .width(viewportWidthDp)
                                .height(scrollbarThickness),
                            scrollValue = effectiveScrollX,
                            maxValue = maxScrollX,
                            isHorizontal = true,
                            onScrollValueChange = { scrollX = it }
                        )
                    } else {
                        Box(
                            modifier = Modifier
                                .width(viewportWidthDp)
                                .height(resizeHandleSize)
                        )
                    }
                    ResizeHandle(
                        modifier = Modifier
                            .width(if (maxScrollY > 0) scrollbarThickness else resizeHandleSize)
                            .height(if (maxScrollX > 0) scrollbarThickness else resizeHandleSize),
                        onResizeStart = {
                            resizeStartWidth = viewportWidthPx
                            resizeStartHeight = viewportHeightPx
                            resizeDragWidth = 0f
                            resizeDragHeight = 0f
                        },
                        onResize = { dragWidth, dragHeight ->
                            resizeDragWidth += dragWidth
                            resizeDragHeight += dragHeight
                            viewportWidthPx = (resizeStartWidth + resizeDragWidth.roundToInt())
                                .coerceIn(minViewportWidthPx, maxViewportWidthPx)
                            viewportHeightPx = (resizeStartHeight + resizeDragHeight.roundToInt())
                                .coerceIn(minViewportHeightPx, maxViewportHeightPx)
                        }
                    )
                }
            }
        }
    }
}

@Composable
private fun ScrollbarTrack(
    modifier: Modifier,
    scrollValue: Int,
    maxValue: Int,
    isHorizontal: Boolean,
    onScrollValueChange: (Int) -> Unit
) {
    var trackSize by remember { mutableStateOf(0) }
    var dragStartScrollValue by remember { mutableStateOf(0) }
    var dragTotal by remember { mutableStateOf(0f) }
    val currentScrollValue by rememberUpdatedState(scrollValue)
    val currentOnScrollValueChange by rememberUpdatedState(onScrollValueChange)
    val density = LocalDensity.current

    Box(
        modifier = modifier
            .background(Color.Black.copy(alpha = 0.2f))
            .onSizeChanged { trackSize = if (isHorizontal) it.width else it.height }
    ) {
        val contentSize = trackSize + maxValue
        val thumbSize = if (contentSize > 0)
            (trackSize * (trackSize.toFloat() / contentSize)).toInt().coerceAtLeast(24)
        else
            trackSize
        val maxThumbOffset = (trackSize - thumbSize).coerceAtLeast(0)
        val thumbOffset = if (maxValue == 0) 0 else (maxThumbOffset * (scrollValue.toFloat() / maxValue)).toInt()

        val thumbModifier = if (isHorizontal) {
            Modifier
                .width(with(density) { thumbSize.toDp() })
                .fillMaxHeight()
                .offset(x = with(density) { thumbOffset.toDp() })
        } else {
            Modifier
                .fillMaxWidth()
                .height(with(density) { thumbSize.toDp() })
                .offset(y = with(density) { thumbOffset.toDp() })
        }.pointerInput(maxValue, maxThumbOffset) {
            detectDragGestures(
                onDragStart = {
                    dragStartScrollValue = currentScrollValue
                    dragTotal = 0f
                },
                onDrag = { change, dragAmount ->
                    change.consume()
                    dragTotal += if (isHorizontal) dragAmount.x else dragAmount.y
                    if (maxThumbOffset > 0) {
                        val next = dragStartScrollValue + (dragTotal * maxValue / maxThumbOffset).roundToInt()
                        currentOnScrollValueChange(next.coerceIn(0, maxValue))
                    }
                }
            )
        }

        Box(thumbModifier.background(Color.White.copy(alpha = 0.75f)))
    }
}

@Composable
private fun ResizeHandle(
    modifier: Modifier,
    onResizeStart: () -> Unit,
    onResize: (dragWidth: Float, dragHeight: Float) -> Unit
) {
    Box(
        modifier = modifier
            .background(Color.Black.copy(alpha = 0.2f))
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragStart = { onResizeStart() },
                    onDrag = { change, dragAmount ->
                        change.consume()
                        onResize(dragAmount.x, dragAmount.y)
                    }
                )
            }
    ) {
        Text(
            text = "◢",
            color = Color.White.copy(alpha = 0.75f),
            modifier = Modifier.align(Alignment.Center)
        )
    }
}

// Smallest usable plugin window. A plugin whose content is smaller than this is not stretched
// past its own content size.
private val MIN_VIEWPORT_DIMENSION = 200.dp

// Floor for a content size, used when a plugin does not tell its UI size.
private const val MIN_CONTENT_FALLBACK_WIDTH = 480
private const val MIN_CONTENT_FALLBACK_HEIGHT = 320

// Fraction of the host area a freshly opened plugin window may occupy.
private const val INITIAL_MAX_HOST_FRACTION = 0.8f

private fun constrainViewport(contentPx: Int, maxAvailablePx: Int, desiredPx: Int, minPx: Int): Int {
    val max = minOf(contentPx, maxAvailablePx).coerceAtLeast(1)
    return desiredPx.coerceIn(minPx.coerceAtMost(max), max)
}
