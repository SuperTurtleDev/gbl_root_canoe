package com.gblbds.manager

import android.os.Bundle
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.fragment.app.FragmentActivity
import com.gblbds.manager.core.AppViewModel
import com.gblbds.manager.core.HardwareKey
import com.gblbds.manager.ui.AppScreen

/** FragmentActivity: the secure-boot BiometricPrompt requires one. */
class MainActivity : FragmentActivity() {
    private val vm: AppViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        vm.start()
        setContent {
            val state by vm.ui.collectAsState()
            val staged by vm.staged.collectAsState()

            val picker = rememberLauncherForActivityResult(
                ActivityResultContracts.OpenDocument()) { uri ->
                // null means the user backed out; the ViewModel clears the
                // request either way so the next picker request is a new one.
                state.requestFile?.let { vm.onFilePicked(it.kind, uri) }
            }

            LaunchedEffect(state.requestFile) {
                state.requestFile?.let { picker.launch(arrayOf(it.mime)) }
            }

            // The biometric prompt is an Activity-scoped UI element: the
            // ViewModel suspends in requestSignature() until this resolves it.
            LaunchedEffect(state.signRequest) {
                state.signRequest?.let { req ->
                    HardwareKey.sign(this@MainActivity, req.digestInfo,
                        object : HardwareKey.SignCallback {
                            override fun onSigned(signature: ByteArray) {
                                vm.onSignResult(signature)
                            }
                            override fun onFailed(message: String) {
                                vm.onSignResult(null)
                            }
                        })
                }
            }

            AppScreen(state = state, staged = staged, vm = vm)
        }
    }
}
