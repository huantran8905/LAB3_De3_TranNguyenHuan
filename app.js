"use strict";
const $ = id => document.getElementById(id);
const HISTORY_MS = 120000;
const STALE_MS = 12000;
const history = [];
let client = null;
let state = null;
let lastState = 0;
let lastSample = 0;
let topic = "";
let pending = null;
let connectionLabel = "Chưa kết nối";

function refreshControls() {
  const fresh = Boolean(client?.connected && state && Date.now() - lastState < STALE_MS);
  $("mode").disabled = !fresh || Boolean(pending);
  $("mode").value = fresh ? state.mode : "";
  for (const [device, name, alarm] of [["pump", "bơm", "heat_alarm"], ["fan", "quạt", "gas_alarm"]]) {
    const on = fresh && state[device] === 1;
    $(device + "-state").textContent = fresh ? (on ? "Đang bật" : "Đang tắt") : "—";
    $(device).textContent = (on ? "Tắt " : "Bật ") + name;
    $(device).disabled = !fresh || state.mode !== "MANUAL" || Boolean(pending) || state[alarm] === 1;
  }
  for (const [id, key] of [["gas-alert", "gas_alarm"], ["heat-alert", "heat_alarm"]]) {
    const active = fresh && state[key] === 1;
    $(id).classList.toggle("alarm", active);
    $(id).textContent = !fresh ? "—" :
      active ? "Vượt ngưỡng" : key === "heat_alarm" && !state.temp_valid ? "Lỗi cảm biến" : "Bình thường";
  }
  const live = Boolean(fresh && lastSample && Date.now() - lastSample < STALE_MS);
  $("connection").textContent = live ? "ESP32 trực tuyến" : connectionLabel;
  if (!live && lastSample && Date.now() - lastSample >= STALE_MS) {
    $("gas-value").textContent = "—";
    $("temp-value").textContent = "—";
    $("gas-detail").textContent = "Đang chờ mẫu mới từ ESP32";
    $("temp-detail").textContent = "Đang chờ mẫu mới từ ESP32";
  }
}

function drawChart(id, key, low, high, threshold, color) {
  const canvas = $(id);
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (!w || !h) return;
  const dpr = window.devicePixelRatio || 1;
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const c = canvas.getContext("2d");
  c.scale(dpr, dpr);
  const left = 29, right = w - 9, top = 9, bottom = h - 22;
  const y = value => bottom - (Math.max(low, Math.min(high, value)) - low) / (high - low) * (bottom - top);
  c.font = "11px system-ui";
  c.fillStyle = "#506461";
  for (const value of [low, (low + high) / 2, high]) {
    c.strokeStyle = "#e1e8e5";
    c.beginPath(); c.moveTo(left, y(value)); c.lineTo(right, y(value)); c.stroke();
    c.fillText(value.toString(), 2, y(value) + 4);
  }
  c.strokeStyle = "#ad6a47";
  c.setLineDash([4, 4]);
  c.beginPath(); c.moveTo(left, y(threshold)); c.lineTo(right, y(threshold)); c.stroke();
  c.setLineDash([]);
  c.fillText("−120 giây", left, h - 4);
  c.textAlign = "right";
  c.fillText("Mẫu mới nhất", right, h - 4);
  if (!history.length) return;
  const end = history.at(-1).at;
  c.strokeStyle = color;
  c.fillStyle = color;
  c.lineWidth = 2.4;
  let previous = null;
  for (const point of history) {
    if (!Number.isFinite(point[key])) {
      previous = null;
      continue;
    }
    const x = right - (end - point.at) / HISTORY_MS * (right - left);
    const cy = y(point[key]);
    if (previous && point.at - previous.at < STALE_MS) {
      c.beginPath(); c.moveTo(previous.x, previous.y); c.lineTo(x, cy); c.stroke();
    }
    c.beginPath(); c.arc(x, cy, 2, 0, 2 * Math.PI); c.fill();
    previous = {x, y: cy, at: point.at};
  }
}

function draw() {
  drawChart("gas-chart", "gas", 0, 100, 3800 / 4095 * 100, "#0a7466");
  drawChart("temp-chart", "temp", -40, 80, 55, "#b25124");
}

function updateSample(data) {
  if (!Number.isInteger(data.gas_adc) || data.gas_adc < 0 || data.gas_adc > 4095 ||
      !Number.isFinite(data.gas_pct) || data.gas_pct < 0 || data.gas_pct > 100 ||
      (data.temp_c !== null && !Number.isFinite(data.temp_c))) return;
  const at = Date.now();
  lastSample = at;
  $("gas-value").textContent = data.gas_pct.toFixed(1);
  $("temp-value").textContent = data.temp_c === null ? "—" : data.temp_c.toFixed(1);
  $("gas-detail").textContent = data.gas_adc + " / 4095 ADC · Ngưỡng bật 92,8%";
  $("temp-detail").textContent = data.temp_c === null ? "DHT22 không trả về số đo" : "Ngưỡng bật 55 °C";
  history.push({at, gas: data.gas_pct, temp: data.temp_c});
  while (history[0].at < at - HISTORY_MS) history.shift();
  draw();
}

function updateState(data) {
  if (!["AUTO", "MANUAL"].includes(data.mode) ||
      !["pump", "fan", "gas_alarm", "heat_alarm", "temp_valid"].every(key => [0, 1].includes(data[key]))) return;
  state = data;
  lastState = Date.now();
  if (pending && data[pending.key] === pending.value) {
    pending = null;
    $("command-status").textContent = "ESP32 đã báo trạng thái yêu cầu.";
  }
}

$("config").addEventListener("submit", event => {
  event.preventDefault();
  if (!window.mqtt) {
    $("notice").textContent = "Chưa tải được MQTT.js. Kiểm tra Internet rồi tải lại trang.";
    return;
  }
  const requestedTopic = $("room").value.trim().replace(/\/+$/, "");
  if (!/^[A-Za-z0-9_-]+(?:\/[A-Za-z0-9_-]+)*$/.test(requestedTopic)) {
    $("notice").textContent = "Nhập topic gốc hợp lệ, ví dụ lab3/de3/nhom17.";
    return;
  }
  const previousClient = client;
  client = null;
  if (previousClient) previousClient.end(true);
  topic = requestedTopic;
  state = pending = null;
  lastState = lastSample = 0;
  history.length = 0;
  $("gas-value").textContent = $("temp-value").textContent = "—";
  $("gas-detail").textContent = "Mức tương đối từ ADC của MQ-2";
  $("temp-detail").textContent = "Cảm biến DHT22";
  draw();
  connectionLabel = "Đang kết nối broker";
  $("notice").textContent = "";
  const session = connectLabBroker();
  client = session;
  session.on("connect", () => {
    if (session !== client) return;
    connectionLabel = "Đã kết nối broker · Chờ ESP32";
    session.subscribe([topic + "/telemetry", topic + "/state"], err => {
      if (session === client) $("notice").textContent = err ? "Đăng ký topic thất bại." : "";
    });
    refreshControls();
  });
  session.on("message", (channel, message, packet) => {
    if (session !== client || packet.retain) return;
    let data;
    try { data = JSON.parse(message.toString()); } catch { return; }
    if (!data || typeof data !== "object") return;
    if (channel === topic + "/telemetry") updateSample(data);
    if (channel === topic + "/state") updateState(data);
    refreshControls();
  });
  session.on("close", () => {
    if (session !== client) return;
    lastState = 0;
    if (pending) {
      pending = null;
      $("command-status").textContent = "Kết nối gián đoạn. Hãy kiểm tra lại trạng thái ESP32.";
    }
    connectionLabel = "Mất kết nối broker · Đang thử lại";
    refreshControls();
  });
  session.on("error", () => {
    if (session === client) $("notice").textContent = "Kết nối MQTT gặp lỗi. Đang chờ kết nối lại.";
  });
  refreshControls();
});

function send(command, key, value) {
  if (!client?.connected || !state || Date.now() - lastState >= STALE_MS || pending) return;
  pending = {key, value, expires: Date.now() + 5000};
  $("command-status").textContent = "Đã yêu cầu " + command + " · Đang chờ trạng thái ESP32";
  const session = client;
  session.publish(topic + "/command", command, {qos: 0, retain: false}, err => {
    if (!err || session !== client) return;
    pending = null;
    $("command-status").textContent = "Gửi lệnh thất bại.";
    refreshControls();
  });
  refreshControls();
}

$("mode").addEventListener("change", event => send(event.target.value, "mode", event.target.value));
$("pump").addEventListener("click", () => send(state.pump ? "PUMP_OFF" : "PUMP_ON", "pump", state.pump ? 0 : 1));
$("fan").addEventListener("click", () => send(state.fan ? "FAN_OFF" : "FAN_ON", "fan", state.fan ? 0 : 1));
setInterval(() => {
  if (pending && Date.now() >= pending.expires) {
    pending = null;
    $("command-status").textContent = "Chưa thấy trạng thái yêu cầu từ ESP32. Kiểm tra kết nối và cảnh báo.";
  }
  refreshControls();
}, 500);
window.addEventListener("resize", draw);
draw();
