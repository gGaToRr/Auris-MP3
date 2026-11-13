package com.auris.app

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.ParcelUuid
import java.util.UUID

object AurisUuids {
    val SERVICE:  UUID = UUID.fromString("a1b2c3d4-0001-4000-8000-00805f9b34fb")
    val CHAR_RX:  UUID = UUID.fromString("a1b2c3d4-0002-4000-8000-00805f9b34fb")
    val CHAR_TX:  UUID = UUID.fromString("a1b2c3d4-0003-4000-8000-00805f9b34fb")
    val CCCD:     UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

enum class AurisState { IDLE, SCANNING, CONNECTING, CONNECTED, READY }

@SuppressLint("MissingPermission")
class AurisBle(
    private val context: Context,
    private val onStateChange: (AurisState) -> Unit,
    private val onNotification: (String) -> Unit,
    private val onLog: (String) -> Unit,
) {
    private val adapter: BluetoothAdapter? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var state = AurisState.IDLE
    private var pendingSubscribe = false

    private fun setState(s: AurisState) {
        state = s
        onStateChange(s)
    }

    // ── Bond state receiver ──

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            val device = intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                ?: return
            val bondState = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1)
            val prevState = intent.getIntExtra(BluetoothDevice.EXTRA_PREVIOUS_BOND_STATE, -1)

            onLog("Bond: ${bondName(prevState)} → ${bondName(bondState)}")

            if (bondState == BluetoothDevice.BOND_BONDED && pendingSubscribe) {
                onLog("Bonded — discovering services...")
                gatt?.discoverServices()
            }
        }
    }

    private fun bondName(state: Int) = when (state) {
        BluetoothDevice.BOND_NONE -> "NONE"
        BluetoothDevice.BOND_BONDING -> "BONDING"
        BluetoothDevice.BOND_BONDED -> "BONDED"
        else -> "?($state)"
    }

    init {
        context.registerReceiver(
            bondReceiver,
            IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED)
        )
    }

    // ── Scan ──

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device.name ?: result.scanRecord?.deviceName
            onLog("Found: ${name ?: "?"} rssi=${result.rssi}")

            if (name == "AURIS") {
                scanner?.stopScan(this)
                setState(AurisState.CONNECTING)
                onLog("Connecting to AURIS...")
                result.device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            onLog("Scan failed: $errorCode")
            setState(AurisState.IDLE)
        }
    }

    fun startScan() {
        if (state != AurisState.IDLE) return
        scanner = adapter?.bluetoothLeScanner
        if (scanner == null) {
            onLog("BLE not available")
            return
        }

        setState(AurisState.SCANNING)
        onLog("Scanning for AURIS...")

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanner?.startScan(null, settings, scanCallback)
    }

    fun stopScan() {
        if (state == AurisState.SCANNING) {
            scanner?.stopScan(scanCallback)
            setState(AurisState.IDLE)
        }
    }

    // ── GATT ──

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                gatt = g
                setState(AurisState.CONNECTED)

                val bond = g.device.bondState
                onLog("Connected (bond=${bondName(bond)})")

                if (bond == BluetoothDevice.BOND_BONDED) {
                    onLog("Already bonded — discovering services...")
                    g.discoverServices()
                } else {
                    onLog("Waiting for pairing...")
                    pendingSubscribe = true
                }
            } else {
                onLog("Disconnected (status=$status)")
                gatt = null
                rxChar = null
                pendingSubscribe = false
                setState(AurisState.IDLE)
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                onLog("Service discovery failed: $status")
                return
            }

            val svc = g.getService(AurisUuids.SERVICE)
            if (svc == null) {
                onLog("Auris service not found!")
                return
            }

            rxChar = svc.getCharacteristic(AurisUuids.CHAR_RX)
            val txChar = svc.getCharacteristic(AurisUuids.CHAR_TX)

            if (rxChar == null || txChar == null) {
                onLog("Characteristics missing (rx=${rxChar != null} tx=${txChar != null})")
                return
            }

            onLog("Services OK — subscribing to notifications...")
            g.setCharacteristicNotification(txChar, true)
            val desc = txChar.getDescriptor(AurisUuids.CCCD)
            if (desc != null) {
                desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION")
                g.writeDescriptor(desc)
            } else {
                onLog("CCCD descriptor not found")
            }

            pendingSubscribe = false
        }

        override fun onDescriptorWrite(g: BluetoothGatt, desc: BluetoothGattDescriptor, status: Int) {
            if (desc.uuid == AurisUuids.CCCD) {
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    onLog("Subscribed — ready")
                    setState(AurisState.READY)
                } else {
                    onLog("CCCD write failed: status=$status — retrying in 1s...")
                    android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                        gatt?.let { retrySubscribe(it) }
                    }, 1000)
                }
            }
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (c.uuid == AurisUuids.CHAR_TX) {
                val msg = c.value?.toString(Charsets.UTF_8) ?: ""
                onLog("← $msg")
                onNotification(msg)
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) {
                onLog("→ sent OK")
            } else {
                onLog("→ write failed: $status")
            }
        }
    }

    private fun retrySubscribe(g: BluetoothGatt) {
        val svc = g.getService(AurisUuids.SERVICE) ?: return
        val txChar = svc.getCharacteristic(AurisUuids.CHAR_TX) ?: return
        onLog("Retrying CCCD subscribe...")
        g.setCharacteristicNotification(txChar, true)
        val desc = txChar.getDescriptor(AurisUuids.CCCD) ?: return
        desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        @Suppress("DEPRECATION")
        g.writeDescriptor(desc)
    }

    // ── Commands ──

    fun sendCommand(cmd: String) {
        val g = gatt ?: return
        val c = rxChar ?: return
        if (state != AurisState.READY && state != AurisState.CONNECTED) return

        onLog("→ $cmd")
        @Suppress("DEPRECATION")
        c.value = cmd.toByteArray(Charsets.UTF_8)
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        @Suppress("DEPRECATION")
        g.writeCharacteristic(c)
    }

    fun disconnect() {
        pendingSubscribe = false
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rxChar = null
        setState(AurisState.IDLE)
        onLog("Disconnected")
    }

    fun destroy() {
        try { context.unregisterReceiver(bondReceiver) } catch (_: Exception) {}
        disconnect()
    }
}
