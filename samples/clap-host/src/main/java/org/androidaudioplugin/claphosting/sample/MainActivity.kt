package org.androidaudioplugin.claphosting.sample

import android.graphics.Rect
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.SurfaceControl
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.material3.Badge
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuAnchorType
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface as MtSurface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupProperties
import kotlin.math.roundToInt
import org.androidaudioplugin.composeaudiocontrols.DefaultKnobTooltip
import org.androidaudioplugin.composeaudiocontrols.DiatonicKeyboardWithControllers
import org.androidaudioplugin.composeaudiocontrols.ImageStripKnob
import org.androidaudioplugin.js.AapAutomationRuntime

class MainActivity : ComponentActivity() {

    companion object {
        private const val TAG = "aap-clap-sample"
        const val FRAME_SIZE  = 1024
        const val SAMPLE_RATE = 48000

        init { System.loadLibrary("sample-clap-host") }
    }

    data class PluginEntry(val entryIdx: Int, val pluginIdx: Int, val label: String)
    data class ParamInfo(val id: Int, val name: String, val min: Float, val max: Float, val default: Float)
    data class PresetItem(val name: String, val loadKey: String)

    @Volatile private var pluginLoaded = false

    private val plugins      = mutableStateListOf<PluginEntry>()
    private var statusText   by mutableStateOf("Discovering plugins…")
    private var isLoading    by mutableStateOf(false)
    private var loadedPlugin by mutableStateOf<PluginEntry?>(null)
    private var hasGui       by mutableStateOf(false)
    private var guiVisible   by mutableStateOf(false)
    private val params       = mutableStateListOf<ParamInfo>()
    private val paramValues  = mutableStateListOf<Float>()
    private val presets      = mutableStateListOf<PresetItem>()
    private var selectedPresetIndex by mutableStateOf(-1)
    // The SurfaceView owned by AudioPluginSurfaceControlClient, returned by nativeShowGui().
    // Tracked as mutableState so Compose re-composes when it changes (null = hidden).
    private var pluginGuiView by mutableStateOf<View?>(null)
    private var guiContentWidthPx by mutableStateOf(0)
    private var guiContentHeightPx by mutableStateOf(0)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
            WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
            WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON or
            WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD
        )

        setContent {
            MaterialTheme {
                MtSurface(modifier = Modifier.fillMaxSize()) {
                    ClapHostScreen()
                }
            }
        }

        Thread {
            val rawList = discoverPlugins(applicationContext)
            bootstrapJsController()
            val discovered = rawList.mapNotNull { s ->
                val i1 = s.indexOf('|')
                val i2 = s.indexOf('|', i1 + 1)
                if (i1 < 0 || i2 < 0) return@mapNotNull null
                PluginEntry(
                    s.substring(0, i1).toInt(),
                    s.substring(i1 + 1, i2).toInt(),
                    s.substring(i2 + 1)
                )
            }
            runOnUiThread {
                plugins.addAll(discovered)
                statusText = if (discovered.isEmpty())
                    "No AAP plugins found. Install some and relaunch."
                else
                    "Select a plugin to play."
            }
        }.start()
    }

    private fun bootstrapJsController() {
        try {
            AapAutomationRuntime.bootstrap(applicationContext)
            AapAutomationRuntime.attachNativeClient(nativeGetAutomationClientHandle())
            AapAutomationRuntime.setPluginCatalog(nativeGetAutomationCatalogJson())
            AapAutomationRuntime.serviceConnector = { packageName ->
                nativeConnectPluginService(packageName)
            }
            Log.i(TAG, "AAP JS automation runtime initialized")
        } catch (e: Throwable) {
            Log.w(TAG, "Failed to initialize AAP JS automation runtime", e)
        }
    }

    private fun startParameterNotificationPolling(plugin: PluginEntry) {
        Thread {
            while (pluginLoaded && loadedPlugin == plugin) {
                Thread.sleep(50)
                val changes = nativeDrainParameterNotifications()
                if (changes.isEmpty())
                    continue

                runOnUiThread {
                    var i = 0
                    var lastParamId = -1
                    var lastValue = 0f
                    while (i + 1 < changes.size) {
                        val paramId = changes[i].toInt()
                        val value = changes[i + 1].toFloat()
                        val index = params.indexOfFirst { it.id == paramId }
                        if (index >= 0 && index < paramValues.size)
                            paramValues[index] = value
                        lastParamId = paramId
                        lastValue = value
                        i += 2
                    }
                    if (lastParamId >= 0)
                        statusText = "Parameter notification: id=$lastParamId value=$lastValue"
                }
            }
        }.start()
    }

    // ---------------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------------

    @Composable
    private fun ClapHostScreen() {
        var lastBackPressed by remember { mutableStateOf(0L) }
        BackHandler {
            if (System.currentTimeMillis() - lastBackPressed < 2000)
                finish()
            else
                Toast.makeText(applicationContext, "Tap once more to quit", Toast.LENGTH_SHORT).show()
            lastBackPressed = System.currentTimeMillis()
        }

        val noteOnStates = remember { List(128) { 0L }.toMutableStateList() }
        Box(modifier = Modifier.fillMaxSize()) {
            Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
                Text(text = statusText, fontSize = 13.sp,
                    modifier = Modifier.padding(bottom = 8.dp))
                PluginDropdown()
                Row(modifier = Modifier.padding(top = 8.dp)) {
                    Button(
                        enabled = hasGui && pluginLoaded && !isLoading,
                        onClick = { toggleGui() },
                        modifier = Modifier.padding(end = 8.dp)
                    ) {
                        Text(if (guiVisible) "Hide UI" else "Show UI")
                    }
                }
                PresetSelector()
                DiatonicKeyboardWithControllers(
                    noteOnStates = noteOnStates.toList(),
                    modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
                    onNoteOn = { note, _ ->
                        if (pluginLoaded) nativeSendNoteOn(note)
                        noteOnStates[note] = 1L
                    },
                    onNoteOff = { note, _ ->
                        if (pluginLoaded) nativeSendNoteOff(note)
                        noteOnStates[note] = 0L
                    }
                )
                ParameterGrid()
            }

            pluginGuiView?.let { guiView ->
                FloatingPluginGui(
                    title = loadedPlugin?.label ?: "Plugin UI",
                    view = guiView,
                    contentWidthPx = guiContentWidthPx,
                    contentHeightPx = guiContentHeightPx,
                    onClose = {
                        pluginGuiView = null
                        guiVisible = false
                        nativeHideGui()
                    }
                )
            }
        }
    }

    @Composable
    private fun FloatingPluginGui(
        title: String,
        view: View,
        contentWidthPx: Int,
        contentHeightPx: Int,
        onClose: () -> Unit
    ) {
        var offsetX by remember { mutableStateOf(0f) }
        var offsetY by remember { mutableStateOf(0f) }
        var scrollX by remember { mutableStateOf(0) }
        var scrollY by remember { mutableStateOf(0) }
        val density = LocalDensity.current
        val scrollbarSize = 12.dp

        BoxWithConstraints(Modifier.fillMaxSize()) {
            val screenWidthPx = with(density) { maxWidth.roundToPx() }
            val screenHeightPx = with(density) { maxHeight.roundToPx() }

            // Viewport is content clamped to available screen space.
            var viewportWidthPx by remember(contentWidthPx, screenWidthPx) {
                mutableStateOf(contentWidthPx.coerceAtMost(screenWidthPx))
            }
            var viewportHeightPx by remember(contentHeightPx, screenHeightPx) {
                mutableStateOf(contentHeightPx.coerceAtMost(screenHeightPx - 120))
            }

            val maxScrollX = (contentWidthPx - viewportWidthPx).coerceAtLeast(0)
            val maxScrollY = (contentHeightPx - viewportHeightPx).coerceAtLeast(0)
            val effectiveScrollX = scrollX.coerceIn(0, maxScrollX)
            val effectiveScrollY = scrollY.coerceIn(0, maxScrollY)

            Popup(
                alignment = Alignment.TopStart,
                offset = IntOffset(offsetX.roundToInt(), offsetY.roundToInt()),
                properties = PopupProperties(clippingEnabled = false)
            ) {
                val viewportWidth: Dp = with(density) { viewportWidthPx.toDp() }
                val viewportHeight: Dp = with(density) { viewportHeightPx.toDp() }
                val contentWidth: Dp = with(density) { contentWidthPx.toDp() }
                val contentHeight: Dp = with(density) { contentHeightPx.toDp() }
                Column(Modifier.padding(10.dp)) {
                    // Draggable title bar
                    Row(Modifier
                        .width(viewportWidth + scrollbarSize)
                        .background(Color.DarkGray.copy(alpha = 0.5f))
                        .border(1.dp, Color.Black)
                        .pointerInput(Unit) {
                            detectDragGestures { change, dragAmount ->
                                change.consume()
                                offsetX += dragAmount.x
                                offsetY += dragAmount.y
                            }
                        }
                    ) {
                        TextButton(onClick = onClose) { Text("[X]") }
                        Text(title, modifier = Modifier.align(Alignment.CenterVertically))
                    }
                    Row {
                        // Viewport box: size the AndroidView to the full content dimensions and
                        // apply a compositor-level crop on the SurfaceView's SurfaceControl so
                        // that only the scrolled viewport region is rendered. A matching
                        // translationX/Y moves that visible region to the Box's screen position.
                        // This follows aaphostsample's pattern (AndroidView = viewport size,
                        // scroll communicated externally); for CLAP we do it host-side via SC crop.
                        Box(Modifier
                            .width(viewportWidth)
                            .height(viewportHeight)
                            .border(1.dp, Color.Black)) {
                            AndroidView(
                                factory = { view },
                                modifier = Modifier
                                    .width(contentWidth)
                                    .height(contentHeight),
                                update = { v ->
                                    v.translationX = -effectiveScrollX.toFloat()
                                    v.translationY = -effectiveScrollY.toFloat()
                                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && v is SurfaceView) {
                                        fun applyCrop(scrollX: Int, scrollY: Int, vpW: Int, vpH: Int) {
                                            val sc = v.surfaceControl ?: return
                                            if (!sc.isValid) return
                                            val crop = Rect(scrollX, scrollY, scrollX + vpW, scrollY + vpH)
                                            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                                                SurfaceControl.Transaction()
                                                    .setCrop(sc, crop)
                                                    .apply()
                                        }
                                        val sx = effectiveScrollX; val sy = effectiveScrollY
                                        val vpW = viewportWidthPx; val vpH = viewportHeightPx
                                        if (v.surfaceControl == null || !v.surfaceControl.isValid) {
                                            // Surface not created yet; apply crop once it is
                                            v.post { applyCrop(sx, sy, vpW, vpH) }
                                        } else {
                                            applyCrop(sx, sy, vpW, vpH)
                                        }
                                    }
                                },
                                onRelease = { v -> (v.parent as? ViewGroup)?.removeView(v) },
                                onReset = {}
                            )
                        }
                        if (maxScrollY > 0) {
                            FloatingGuiScrollbarTrack(
                                modifier = Modifier.width(scrollbarSize).height(viewportHeight),
                                scrollValue = effectiveScrollY,
                                maxValue = maxScrollY,
                                isHorizontal = false,
                                onScrollValueChange = { scrollY = it }
                            )
                        }
                    }
                    Row {
                        if (maxScrollX > 0) {
                            FloatingGuiScrollbarTrack(
                                modifier = Modifier.width(viewportWidth).height(scrollbarSize),
                                scrollValue = effectiveScrollX,
                                maxValue = maxScrollX,
                                isHorizontal = true,
                                onScrollValueChange = { scrollX = it }
                            )
                        } else {
                            Spacer(Modifier.width(viewportWidth).height(scrollbarSize))
                        }
                        FloatingGuiResizeHandle(
                            modifier = Modifier.width(scrollbarSize).height(scrollbarSize),
                            viewportWidthPx = viewportWidthPx,
                            viewportHeightPx = viewportHeightPx,
                            onResize = { w, h ->
                                viewportWidthPx = w.coerceAtMost(contentWidthPx)
                                viewportHeightPx = h.coerceAtMost(contentHeightPx)
                            }
                        )
                    }
                }
            }
        }
    }

    @Composable
    private fun FloatingGuiScrollbarTrack(
        modifier: Modifier,
        scrollValue: Int,
        maxValue: Int,
        isHorizontal: Boolean,
        onScrollValueChange: (Int) -> Unit
    ) {
        var dragStartScrollValue by remember { mutableStateOf(0) }
        var dragTotal by remember { mutableStateOf(0f) }
        val currentScrollValue by rememberUpdatedState(scrollValue)
        val currentOnScrollValueChange by rememberUpdatedState(onScrollValueChange)

        BoxWithConstraints(modifier.background(Color(0xFF808080))) {
            val trackSize = if (isHorizontal) constraints.maxWidth else constraints.maxHeight
            val contentSize = trackSize + maxValue
            val thumbSize = if (contentSize > 0)
                (trackSize * (trackSize.toFloat() / contentSize)).toInt().coerceAtLeast(24)
            else
                trackSize
            val maxThumbOffset = (trackSize - thumbSize).coerceAtLeast(0)
            val thumbOffset = if (maxValue == 0 || maxThumbOffset == 0) 0
                else (maxThumbOffset * (scrollValue.toFloat() / maxValue)).toInt()

            val density = LocalDensity.current
            val thumbModifier = if (isHorizontal) {
                Modifier
                    .width(with(density) { thumbSize.toDp() })
                    .fillMaxHeight()
                    .offset(x = with(density) { thumbOffset.toDp() })
                    .pointerInput(maxValue, maxThumbOffset) {
                        detectDragGestures(
                            onDragStart = { dragStartScrollValue = currentScrollValue; dragTotal = 0f },
                            onDrag = { change, dragAmount ->
                                change.consume()
                                dragTotal += dragAmount.x
                                if (maxThumbOffset > 0)
                                    currentOnScrollValueChange(
                                        (dragStartScrollValue + (dragTotal * maxValue / maxThumbOffset).roundToInt()).coerceIn(0, maxValue))
                            }
                        )
                    }
            } else {
                Modifier
                    .fillMaxWidth()
                    .height(with(density) { thumbSize.toDp() })
                    .offset(y = with(density) { thumbOffset.toDp() })
                    .pointerInput(maxValue, maxThumbOffset) {
                        detectDragGestures(
                            onDragStart = { dragStartScrollValue = currentScrollValue; dragTotal = 0f },
                            onDrag = { change, dragAmount ->
                                change.consume()
                                dragTotal += dragAmount.y
                                if (maxThumbOffset > 0)
                                    currentOnScrollValueChange(
                                        (dragStartScrollValue + (dragTotal * maxValue / maxThumbOffset).roundToInt()).coerceIn(0, maxValue))
                            }
                        )
                    }
            }
            Box(thumbModifier.background(Color(0xFFCCCCCC)))
        }
    }

    @Composable
    private fun FloatingGuiResizeHandle(
        modifier: Modifier,
        viewportWidthPx: Int,
        viewportHeightPx: Int,
        onResize: (Int, Int) -> Unit
    ) {
        val currentW by rememberUpdatedState(viewportWidthPx)
        val currentH by rememberUpdatedState(viewportHeightPx)
        var dragStartW by remember { mutableStateOf(0) }
        var dragStartH by remember { mutableStateOf(0) }
        var totalDragX by remember { mutableStateOf(0f) }
        var totalDragY by remember { mutableStateOf(0f) }

        Box(modifier
            .background(Color.DarkGray.copy(alpha = 0.5f))
            .pointerInput(Unit) {
                detectDragGestures(
                    onDragStart = {
                        dragStartW = currentW
                        dragStartH = currentH
                        totalDragX = 0f
                        totalDragY = 0f
                    },
                    onDrag = { change, dragAmount ->
                        change.consume()
                        totalDragX += dragAmount.x
                        totalDragY += dragAmount.y
                        val newW = (dragStartW + totalDragX.roundToInt()).coerceAtLeast(100)
                        val newH = (dragStartH + totalDragY.roundToInt()).coerceAtLeast(100)
                        onResize(newW, newH)
                    }
                )
            }
        ) {
            Canvas(Modifier.matchParentSize()) {
                val lineColor = Color.White.copy(alpha = 0.7f)
                for (i in 1..3) {
                    val t = i * (size.width / 4f)
                    drawLine(lineColor, Offset(t, size.height), Offset(size.width, t),
                        strokeWidth = 1.5f, cap = StrokeCap.Round)
                }
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun PresetSelector() {
        if (presets.isEmpty()) return
        var expanded by remember { mutableStateOf(false) }
        val label = if (selectedPresetIndex in presets.indices)
            presets[selectedPresetIndex].name else "— select preset —"

        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it },
            modifier = Modifier.padding(top = 8.dp)
        ) {
            TextField(
                value = label,
                onValueChange = {},
                readOnly = true,
                label = { Text("Preset") },
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
                modifier = Modifier
                    .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                    .fillMaxWidth()
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                presets.forEachIndexed { index, preset ->
                    DropdownMenuItem(
                        text = { Text(preset.name, fontSize = 12.sp) },
                        onClick = {
                            expanded = false
                            selectedPresetIndex = index
                            Thread {
                                nativeSetPreset(preset.loadKey)
                                var stats = ""
                                repeat(10) {
                                    Thread.sleep(50)
                                    val values = nativeGetParameterValues()
                                    stats = nativeGetParameterNotificationStats()
                                    runOnUiThread {
                                        for (i in values.indices)
                                            if (i < paramValues.size)
                                                paramValues[i] = values[i]
                                        statusText = "Preset: ${preset.name}  [$stats]"
                                    }
                                }
                            }.start()
                        }
                    )
                }
            }
        }
    }

    @Composable
    private fun ParameterGrid() {
        if (params.isEmpty()) return
        LazyVerticalGrid(
            columns = GridCells.Fixed(2),
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(max = 200.dp)
                .padding(top = 8.dp)
        ) {
            items(params.size) { index ->
                val param = params[index]
                Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                    Badge(modifier = Modifier.width(100.dp).padding(4.dp, 0.dp)) {
                        Text("${param.id}: ${param.name}", fontSize = 10.sp)
                    }
                    ImageStripKnob(
                        drawableResId = R.drawable.bright_life,
                        value = paramValues.getOrElse(index) { param.default },
                        valueRange = param.min..param.max,
                        tooltip = { DefaultKnobTooltip(showTooltip = true, value = knobValue) },
                        onValueChange = { v ->
                            if (index < paramValues.size) paramValues[index] = v
                            nativeSendParameterChange(param.id, v.toDouble())
                        }
                    )
                }
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    private fun PluginDropdown() {
        var expanded by remember { mutableStateOf(false) }
        val selectedLabel = loadedPlugin?.label ?: if (isLoading) "Loading…" else "— select plugin —"

        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { if (!isLoading) expanded = it }
        ) {
            TextField(
                value = selectedLabel,
                onValueChange = {},
                readOnly = true,
                label = { Text("Plugin") },
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded) },
                modifier = Modifier
                    .menuAnchor(ExposedDropdownMenuAnchorType.PrimaryNotEditable)
                    .fillMaxWidth()
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                plugins.forEach { plugin ->
                    DropdownMenuItem(
                        text = { Text(plugin.label, fontSize = 12.sp) },
                        onClick = {
                            expanded = false
                            selectPlugin(plugin)
                        }
                    )
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Plugin lifecycle
    // ---------------------------------------------------------------------------

    private fun toggleGui() {
        if (guiVisible) {
            pluginGuiView = null   // drop the view first so Compose disposes AndroidView
            guiVisible = false
            nativeHideGui()
        } else {
            // Run AAPXS Binder IPC (guiCreate/guiSetParent/guiShow) on a background
            // thread, then retrieve the SurfaceView on the main thread (Android requires
            // Views to be created on the main thread).
            Thread {
                val ok = nativeShowGui()
                // guiGetSize must be called from the CLAP "main" thread (this background thread).
                val size = if (ok) nativeGetGuiSize() else LongArray(2)
                runOnUiThread {
                    if (ok) {
                        guiContentWidthPx = if (size.size >= 2 && size[0] > 0) size[0].toInt() else 960
                        guiContentHeightPx = if (size.size >= 2 && size[1] > 0) size[1].toInt() else 600
                        @Suppress("UNCHECKED_CAST")
                        val view = nativeGetGuiView() as? View
                        pluginGuiView = view
                        guiVisible = pluginGuiView != null
                    }
                }
            }.start()
        }
    }

    private fun selectPlugin(plugin: PluginEntry) {
        if (isLoading || loadedPlugin == plugin) return

        if (pluginLoaded) {
            pluginGuiView = null   // drop view before unloading native plugin
            guiVisible = false
            hasGui = false
            pluginLoaded = false
            params.clear()
            paramValues.clear()
            presets.clear()
            selectedPresetIndex = -1
            nativeUnloadPlugin()
            loadedPlugin = null
        }

        isLoading = true
        statusText = "Loading ${plugin.label}…"
        Thread {
            val ok = nativeLoadPlugin(plugin.entryIdx, plugin.pluginIdx, SAMPLE_RATE, FRAME_SIZE)
            // Query params/presets on the loader thread — the CLAP host captures this thread
            // as "main", so paramsCount/paramsGetInfo must be called from here.
            val rawParams = if (ok) nativeGetParameters() else emptyArray()
            val rawPresets = if (ok) nativeGetPresets() else emptyArray()
            runOnUiThread {
                isLoading = false
                if (ok) {
                    pluginLoaded = true
                    loadedPlugin = plugin
                    hasGui = nativePluginHasGui()
                    statusText = "Playing: ${plugin.label}" +
                                 if (hasGui) "  [has GUI]" else ""
                    val parsed = rawParams.mapNotNull { s ->
                        val parts = s?.split("|") ?: return@mapNotNull null
                        if (parts.size < 5) return@mapNotNull null
                        ParamInfo(
                            id      = parts[0].toIntOrNull() ?: return@mapNotNull null,
                            name    = parts[1],
                            min     = parts[2].toFloatOrNull() ?: 0f,
                            max     = parts[3].toFloatOrNull() ?: 1f,
                            default = parts[4].toFloatOrNull() ?: 0f
                        )
                    }
                    params.clear()
                    params.addAll(parsed)
                    paramValues.clear()
                    paramValues.addAll(parsed.map { it.default })
                    presets.clear()
                    presets.addAll(rawPresets.mapNotNull { s ->
                        if (s == null) return@mapNotNull null
                        val tab = s.indexOf('\t')
                        if (tab < 0) PresetItem(s, s)
                        else PresetItem(s.substring(0, tab), s.substring(tab + 1))
                    })
                    selectedPresetIndex = -1
                    startParameterNotificationPolling(plugin)
                } else {
                    statusText = "Failed to load ${plugin.label}"
                }
            }
        }.start()
    }

    override fun onDestroy() {
        super.onDestroy()
        if (pluginLoaded) {
            pluginLoaded = false
            nativeUnloadPlugin()
        }
    }

    // ---------------------------------------------------------------------------
    // JNI
    // ---------------------------------------------------------------------------

    private external fun discoverPlugins(context: android.content.Context): Array<String>
    private external fun nativeLoadPlugin(entryIndex: Int, pluginIndex: Int,
                                          sampleRate: Int, frameSize: Int): Boolean
    private external fun nativeSendNoteOn(key: Int)
    private external fun nativeSendNoteOff(key: Int)
    private external fun nativeUnloadPlugin()
    private external fun nativePluginHasGui(): Boolean
    private external fun nativeShowGui(): Boolean
    private external fun nativeGetGuiView(): Any?
    private external fun nativeGetGuiSize(): LongArray
    private external fun nativeHideGui()
    private external fun nativeGetParameters(): Array<String?>
    private external fun nativeGetParameterValues(): FloatArray
    private external fun nativeGetParameterNotificationStats(): String
    private external fun nativeDrainParameterNotifications(): DoubleArray
    private external fun nativeSendParameterChange(paramId: Int, value: Double)
    private external fun nativeGetPresets(): Array<String?>
    private external fun nativeSetPreset(loadKey: String)
    private external fun nativeGetAutomationClientHandle(): Long
    private external fun nativeGetAutomationCatalogJson(): String
    private external fun nativeConnectPluginService(packageName: String): Boolean
}
