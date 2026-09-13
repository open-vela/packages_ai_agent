# MQTT IoT Control

Control IoT devices through MQTT message bus.

## When to use
When user mentions IoT devices, smart home via MQTT, sensor data,
or wants to send/receive MQTT messages.

## How to use
AI Agent connects to MQTT broker and listens on agent/in topic.
Outbound messages go to agent/out topic.

1. Check MQTT config: read_file /data/ai_agent/config/config.json
   Look for mqtt_broker, mqtt_topic_in, mqtt_topic_out
2. If not configured: tell user to run set_mqtt <host:port>
3. To send a command to IoT device:
   The agent's reply on the MQTT channel is automatically published
   to agent/out. Format the reply as JSON that the IoT device expects.
4. To check device status: ask user to have the device publish
   status to agent/in, then parse the incoming message.

## Message format
- Inbound (agent/in): {"type":"message","content":"<text>","chat_id":"<device_id>"}
- Outbound (agent/out): {"type":"response","content":"<reply>","chat_id":"<device_id>"}

## Example
User (via MQTT): "Turn on the light"
Agent reply (auto-published to agent/out):
{"type":"response","content":"{\"action\":\"set\",\"device\":\"light\",\"state\":\"on\"}","chat_id":"device1"}
