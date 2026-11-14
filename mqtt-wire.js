"use strict";

function connectLabBroker() {
  return mqtt.connect("wss://broker.hivemq.com:8884/mqtt", {
    reconnectPeriod: 3000,
    connectTimeout: 8000,
    queueQoSZero: false
  });
}
