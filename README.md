# ArvaCore Domain Logic & AWD Decision Engine

This directory contains the central domain model and decision engine for **ArvaCore**, grounded in the research paper:

> **"ARVACORE: An IoT-Based Irrigation Monitoring and Automated Water Management System for Rice Cultivation"**  
> *Polytechnic University of the Philippines, Manila (Capucion, Colle, Del Rosario, Ramirez; September 2026)*

## Core Modules

1. **`arvacore_engine.py`**:
   - **`NodeReading`**: Physical sensor validation for JSN-SR04T ultrasonic still-well distance ($20 - 450\,\text{cm}$) and capacitive soil moisture ($0 - 100\%$).
   - **Still-Well Geometry**: Calculates actual water level relative to field soil surface datum:
     $$\text{Water Level}\,(\text{cm}) = \text{Still-Well Datum Offset}\,(20.0\,\text{cm}) - \text{Measured Distance}\,(\text{cm})$$
   - **`ArvaCoreDecisionEngine`**: Evaluates AWD thresholds:
     - **Safe Drying**: Water depth between $0\,\text{cm}$ and $-15\,\text{cm}$. Status: `SAFE_DRYING`, Command: `HOLD`.
     - **Reflood Trigger**: Water depth $\le -15\,\text{cm}$. Status: `REFLOOD_REQUIRED`, Command: `REFLOOD`.
     - **Flooded Optimal**: Water depth $\ge +5\,\text{cm}$. Status: `FLOODED_OPTIMAL`, Command: `HOLD`.
     - **Critical Dryness**: Water depth $\le -20\,\text{cm}$ or soil moisture $< 30\%$. Status: `CRITICAL_DRY`, Command: `REFLOOD`.
   - **`FlowVerificationSubsystem`**: Closed-loop verification of water delivery using the central pipeline flow sensor. Distinguishes pump command state from physical flow, detecting dry-runs and leaks.

2. **`test_arvacore_engine.py`**:
   - Comprehensive test suite validating AWD transitions, sensor bound rejections, still-well geometry, and flow anomalies.

## Running Tests
```bash
python3 -m unittest test_arvacore_engine.py
```

