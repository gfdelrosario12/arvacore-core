# ArvaCore Domain Logic & AWD Decision Engine

This directory contains the central domain model and decision engine for **ArvaCore**, grounded in the research paper:

> **"ARVACORE: An IoT-Based Irrigation Monitoring and Automated Water Management System for Rice Cultivation"**  
> *Polytechnic University of the Philippines, Manila (Capucion, Colle, Del Rosario, Ramirez; September 2026)*

## Core Modules

1. **`arvacore_engine.py`**:
   - **`NodeReading`**: Physical sensor validation for JSN-SR04T ultrasonic still-well distance ($20.0 - 450.0\,\text{cm}$) and capacitive soil moisture ($0.0 - 100.0\%$). Strictly validates physical bounds and rejects synthetic fallbacks or out-of-range sensor readings.
   - **Still-Well Geometry**: Calculates actual water level relative to field soil surface datum:
     $$\text{Water Level}\,(\text{cm}) = \text{Still-Well Datum Offset}\,(20.0\,\text{cm}) - \text{Measured Distance}\,(\text{cm})$$
     - $d = 20.0\,\text{cm} \implies h = 0.0\,\text{cm}$ (Field soil surface level)
     - $d = 35.0\,\text{cm} \implies h = -15.0\,\text{cm}$ (AWD reflood threshold)
     - $d = 15.0\,\text{cm} \implies h = +5.0\,\text{cm}$ (Target ponded flood depth)
   - **`ArvaCoreDecisionEngine`**: Evaluates Alternate Wetting and Drying (AWD) thresholds:
     - **Safe Drying**: Water depth between $0.0\,\text{cm}$ and $-15.0\,\text{cm}$. Status: `SAFE_DRYING`, Command: `HOLD`.
     - **Reflood Trigger**: Water depth $\le -15.0\,\text{cm}$. Status: `REFLOOD_REQUIRED`, Command: `REFLOOD`.
     - **Flooded Optimal**: Water depth $\ge +5.0\,\text{cm}$. Status: `FLOODED_OPTIMAL`, Command: `HOLD`.
     - **Critical Dryness**: Water depth $\le -20.0\,\text{cm}$ or soil moisture $< 30.0\%$. Status: `CRITICAL_DRY`, Command: `REFLOOD`.
   - **`FlowVerificationSubsystem`**: Closed-loop verification of water delivery using the central pipeline Hall-effect flow sensor.
     - **Physical Delivery Verification**: When the central pump is commanded `ON`, physical water flow MUST register $\ge 5.0\,\text{L/min}$ within the verification window.
     - **Fault & Anomaly Detection**: If flow remains $< 5.0\,\text{L/min}$ while commanded `ON`, the system detects a dry-run / pump blockage condition, raises an operational fault, and halts actuation.
     - **Uncommanded Flow / Leak Detection**: If flow registers $\ge 5.0\,\text{L/min}$ while the pump is commanded `OFF`, a pipeline leak or valve bypass anomaly is flagged.

2. **`test_arvacore_engine.py`**:
   - Comprehensive test suite validating AWD transitions, sensor bound rejections, still-well geometry, and closed-loop flow anomalies.
   - 6/6 unit tests passing.

3. **`sketch_pump_controller/sketch_pump_controller.ino`**:
   - ESP32 central pump and solenoid valve actuation controller.
   - Active-LOW relay pin assignments:
     - Pump Relay: `GPIO 26`
     - Solenoid Valve Relay: `GPIO 27`
     - Hall-Effect Flow Sensor: `GPIO 4` (Interrupt pulse counter)
     - Heartbeat / Status LED: `GPIO 2`
   - Connects to deployed backend (`https://api-aquaflow.gladwindr.me/api/v1`) with gateway key authentication.
   - Enforces closed-loop flow delivery verification, dry-run safety shutoff, and maximum runtime timeout guards.

## Running Tests
```bash
python3 -m unittest test_arvacore_engine.py
```

