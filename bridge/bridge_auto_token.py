"""
PAMSIMAS - AES Decrypt Bridge (AUTO TOKEN via ThingsBoard REST API)
Subscribe ke Mosquitto lokal, dekripsi AES-128, lalu forward ke ThingsBoard.
Token device TIDAK perlu diisi manual — otomatis dicari lewat REST API
ThingsBoard begitu ada node baru terdeteksi, mirip konsep device provisioning.

Install dulu:
    pip install paho-mqtt pycryptodome requests

Jalankan:
    python bridge.py
"""

import base64
import json
import os
import ssl
import requests
from Crypto.Cipher import AES
from Crypto.Util.Padding import unpad
import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish

# ================== KONFIGURASI ==================
LOCAL_BROKER = "localhost"
LOCAL_PORT   = 1883
LOCAL_TOPIC  = "sensor/encrypted/#"

TB_HOST = "thingsboard.cloud"
TB_PORT = 8883                     # MQTT+TLS untuk publish telemetry
TB_REST_URL = "https://thingsboard.cloud"  # REST API (HTTPS biasa)

# Akun TENANT ADMIN ThingsBoard (bukan token device) — dipakai HANYA
# untuk mencari tahu token device secara otomatis lewat REST API
TB_ADMIN_EMAIL = "xxxx@gmail.com"
TB_ADMIN_PASSWORD = "xxxx"

# HARUS SAMA PERSIS dengan aesKey[16] di firmware SEMUA node
AES_KEY = b"xxxxx"

TOKEN_CACHE_FILE = "node_token_cache.json"
# ===================================================


def load_token_cache() -> dict:
    if os.path.exists(TOKEN_CACHE_FILE):
        with open(TOKEN_CACHE_FILE, "r") as f:
            return json.load(f)
    return {}


def save_token_cache(cache: dict):
    with open(TOKEN_CACHE_FILE, "w") as f:
        json.dump(cache, f, indent=2)


token_cache = load_token_cache()

tb_jwt = None


def tb_login():
    global tb_jwt
    resp = requests.post(
        f"{TB_REST_URL}/api/auth/login",
        json={"username": TB_ADMIN_EMAIL, "password": TB_ADMIN_PASSWORD},
        timeout=10,
    )
    resp.raise_for_status()
    tb_jwt = resp.json()["token"]
    print("[Bridge] Login ThingsBoard REST API berhasil")


def tb_auth_headers():
    return {"X-Authorization": f"Bearer {tb_jwt}"}


def fetch_token_for_node(node_id: str):
    """Cari device 'Node <id>' di ThingsBoard, ambil token aksesnya.
    Dipanggil otomatis saat node baru pertama kali kirim data."""
    global tb_jwt
    device_name = f"Node {node_id}"

    for attempt in range(2):
        if tb_jwt is None:
            tb_login()

        r = requests.get(
            f"{TB_REST_URL}/api/tenant/devices",
            params={"deviceName": device_name},
            headers=tb_auth_headers(),
            timeout=10,
        )
        if r.status_code == 401:
            tb_jwt = None
            continue
        if r.status_code == 404:
            print(f"[Bridge] Device '{device_name}' belum terdaftar di ThingsBoard")
            return None
        r.raise_for_status()
        device_id = r.json()["id"]["id"]

        r2 = requests.get(
            f"{TB_REST_URL}/api/device/{device_id}/credentials",
            headers=tb_auth_headers(),
            timeout=10,
        )
        if r2.status_code == 401:
            tb_jwt = None
            continue
        r2.raise_for_status()
        token = r2.json()["credentialsId"]

        print(f"[Bridge] Token untuk '{device_name}' berhasil ditemukan otomatis")
        return token

    print(f"[Bridge] Gagal ambil token untuk '{device_name}' setelah beberapa percobaan")
    return None


def get_token_for_node(node_id: str):
    if node_id in token_cache:
        return token_cache[node_id]

    token = fetch_token_for_node(node_id)
    if token:
        token_cache[node_id] = token
        save_token_cache(token_cache)
    return token


def aes_decrypt(b64_payload: str) -> str:
    raw = base64.b64decode(b64_payload)
    iv, ciphertext = raw[:16], raw[16:]
    cipher = AES.new(AES_KEY, AES.MODE_CBC, iv)
    padded = cipher.decrypt(ciphertext)
    return unpad(padded, 16).decode("utf-8")


def publish_to_tb(node_id: str, data: dict):
    token = get_token_for_node(node_id)
    if token is None:
        print(f"[Bridge] Node {node_id} tidak bisa dikenali, data dibuang")
        return

    publish.single(
        topic="v1/devices/me/telemetry",
        payload=json.dumps(data),
        hostname=TB_HOST,
        port=TB_PORT,
        auth={"username": token, "password": None},
        tls={"cert_reqs": ssl.CERT_NONE, "insecure": True},
        protocol=mqtt.MQTTv311,
    )


def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"[Bridge] Terhubung ke Mosquitto lokal, subscribe '{LOCAL_TOPIC}'")
        client.subscribe(LOCAL_TOPIC)
    else:
        print(f"[Bridge] Gagal connect ke Mosquitto, rc={rc}")


def on_message(client, userdata, msg):
    try:
        node_id = msg.topic.split("/")[-1]
        payload_str = msg.payload.decode("utf-8").strip()
        print(f"[Bridge] Node {node_id} - TERIMA payload terenkripsi: {payload_str}")

        if payload_str.startswith("ENC:"):
            payload_str = payload_str[4:]

        plaintext = aes_decrypt(payload_str)
        print(f"[Bridge] Node {node_id} - HASIL decrypt: {plaintext}")

        data = json.loads(plaintext)
        publish_to_tb(node_id, data)
        print(f"[Bridge] Node {node_id} - Diteruskan ke ThingsBoard: {data}")

    except Exception as e:
        print(f"[Bridge] GAGAL memproses pesan dari topik {msg.topic}: {e}")


local_client = mqtt.Client(client_id="pamsimas_bridge_local")
local_client.on_connect = on_connect
local_client.on_message = on_message


def main():
    tb_login()
    print(f"[Bridge] Connecting ke Mosquitto lokal {LOCAL_BROKER}:{LOCAL_PORT} ...")
    local_client.connect(LOCAL_BROKER, LOCAL_PORT, keepalive=60)
    print("[Bridge] Siap! Token device dicari OTOMATIS saat node baru terdeteksi.")
    local_client.loop_forever()


if __name__ == "__main__":
    main()
