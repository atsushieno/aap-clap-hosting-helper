package org.androidaudioplugin.claphosting.sample

import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
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
import androidx.compose.material3.TextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
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
    private val params       = mutableStateListOf<ParamInfo>()
    private val paramValues  = mutableStateListOf<Float>()
    private val presets      = mutableStateListOf<PresetItem>()
    private var selectedPresetIndex by mutableStateOf(-1)
    // The plugin UI: its SurfaceView and content size (0 if the plugin does not tell it). null = hidden.
    data class PluginGui(val view: View, val contentWidth: Int, val contentHeight: Int)
    private var pluginGui by mutableStateOf<PluginGui?>(null)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        @Suppress("DEPRECATION")
        window.addFlags(
            WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
            WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or
            WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON or
            WindowManager.LayoutParams.FLAG_DISMISS_KEYGUARD
        )

        enableEdgeToEdge()
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
                // The plugin's main-thread work (e.g. parameter metadata refresh) runs on the UI
                // thread, which also unloads the plugin.
                runOnUiThread {
                    if (!pluginLoaded || loadedPlugin != plugin)
                        return@runOnUiThread
                    if (nativeIdle())
                        refreshParameters()
                    val size = IntArray(2)
                    if (nativeTakeGuiResizeRequest(size))
                        pluginGui = pluginGui?.copy(contentWidth = size[0], contentHeight = size[1])
                }
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

    private fun parseParameters(rawParams: Array<String?>) = rawParams.mapNotNull { s ->
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

    // Re-reads the parameter list after the plugin rescanned its parameters.
    private fun refreshParameters() {
        val parsed = parseParameters(nativeGetParameters())
        val values = nativeGetParameterValues()
        params.clear()
        params.addAll(parsed)
        paramValues.clear()
        paramValues.addAll(parsed.indices.map { values.getOrElse(it) { parsed[it].default } })
        statusText = "Parameters updated (${parsed.size})"
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
        // Keep the controls (and the floating plugin UI) out of the system bars and cutouts,
        // where touches would go to the system instead.
        Box(modifier = Modifier.fillMaxSize().safeDrawingPadding()) {
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
                        Text(if (pluginGui != null) "Hide UI" else "Show UI")
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

            pluginGui?.let { gui ->
                // Keyed so that a new plugin UI gets a fresh window state.
                key(gui.view) {
                    PluginGuiWindow(
                        title = loadedPlugin?.label ?: "Plugin UI",
                        surfaceView = gui.view,
                        preferredContentWidth = gui.contentWidth,
                        preferredContentHeight = gui.contentHeight,
                        onConfigureViewport = { vw, vh, cw, ch, sx, sy ->
                            nativeConfigureGuiViewport(vw, vh, cw, ch, sx, sy)
                        },
                        onShow = {
                            if (!nativeShowGui())
                                statusText = "Failed to show the plugin UI"
                        },
                        onClose = { closeGui() }
                    )
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
                                repeat(10) {
                                    Thread.sleep(50)
                                    // Parameter values are read on the UI thread, which is
                                    // where the plugin's main-thread work happens too.
                                    runOnUiThread {
                                        if (!pluginLoaded)
                                            return@runOnUiThread
                                        val values = nativeGetParameterValues()
                                        val stats = nativeGetParameterNotificationStats()
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

    // The CLAP GUI calls are made on the UI thread, which is also the CLAP main thread here.
    private fun toggleGui() {
        if (pluginGui != null) {
            closeGui()
            return
        }
        val size = IntArray(2)
        val view = nativeCreateGui(size) as? View
        if (view == null) {
            statusText = "Failed to create the plugin UI"
            return
        }
        pluginGui = PluginGui(view, size[0], size[1])
    }

    private fun closeGui() {
        if (pluginGui == null)
            return
        pluginGui = null   // drop the view first so Compose disposes AndroidView
        nativeHideGui()
    }

    private fun selectPlugin(plugin: PluginEntry) {
        if (isLoading || loadedPlugin == plugin) return

        if (pluginLoaded) {
            pluginGui = null   // drop view before unloading native plugin
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
                    val parsed = parseParameters(rawParams)
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
    private external fun nativeCreateGui(size: IntArray): Any?
    private external fun nativeConfigureGuiViewport(viewportWidth: Int, viewportHeight: Int,
                                                    contentWidth: Int, contentHeight: Int,
                                                    scrollX: Int, scrollY: Int): Boolean
    private external fun nativeShowGui(): Boolean
    private external fun nativeHideGui()
    private external fun nativeTakeGuiResizeRequest(size: IntArray): Boolean
    private external fun nativeIdle(): Boolean
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
