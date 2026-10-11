"""
ArvaCore Domain Engine - Python Package
Grounding: PUP BS Computer Engineering Thesis (Sept 2026)
"ARVACORE: An IoT-Based Irrigation Monitoring and Automated Water Management System for Rice Cultivation"
Authors: Capucion, Colle, Del Rosario, Ramirez

Responsibilities:
- Physical sensor telemetry validation (bounds checking, plausibility, still-well geometry)
- Multi-node field condition aggregation
- Alternate Wetting and Drying (AWD) threshold evaluations & state transitions:
    - Safe drying (water level drop down to -15 cm / safe threshold)
    - Reflood trigger (when water level reaches -15 cm, reflood up to +5 cm)
    - Soil moisture correlation
    - Flow verification (pump active vs flow sensor pulses)
    - Command lifecycle and safety interlocks (HOLD, REFLOOD, SKIP, EMERGENCY_STOP)
"""

from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Dict, Any
from datetime import datetime


class AWDState(str, Enum):
    SAFE_DRYING = "SAFE_DRYING"
    REFLOOD_REQUIRED = "REFLOOD_REQUIRED"
    FLOODED_OPTIMAL = "FLOODED_OPTIMAL"
    CRITICAL_DRY = "CRITICAL_DRY"
    EMERGENCY_STOP = "EMERGENCY_STOP"


class IrrigationCommandType(str, Enum):
    REFLOOD = "REFLOOD"
    HOLD = "HOLD"
    SKIP = "SKIP"
    EMERGENCY_STOP = "EMERGENCY_STOP"


class CommandExecutionState(str, Enum):
    REQUESTED = "REQUESTED"
    DISPATCHED = "DISPATCHED"
    ACKNOWLEDGED = "ACKNOWLEDGED"
    VERIFIED_FLOWING = "VERIFIED_FLOWING"
    COMPLETED = "COMPLETED"
    FAILED = "FAILED"
    ABORTED = "ABORTED"


@dataclass
class NodeReading:
    node_id: str
    timestamp: datetime
    distance_cm: float  # Raw ultrasonic distance to float in still-well (20 - 450 cm)
    soil_moisture_percent: float  # 0 - 100%
    battery_voltage: float = 4.2
    temperature_c: Optional[float] = None
    humidity_percent: Optional[float] = None

    @property
    def is_valid_distance(self) -> bool:
        return 20.0 <= self.distance_cm <= 450.0

    @property
    def is_valid_soil_moisture(self) -> bool:
        return 0.0 <= self.soil_moisture_percent <= 100.0

    @property
    def water_level_cm(self) -> float:
        """
        Still-well geometry: PVC pipe datum height above soil surface = 20.0 cm.
        When distance is 20.0 cm, water level is at ground level (0.0 cm).
        When distance is 35.0 cm, water level is 15 cm below ground (-15.0 cm).
        """
        return 20.0 - self.distance_cm


@dataclass
class AWDThresholdConfig:
    reflood_trigger_depth_cm: float = -15.0  # Standard AWD threshold: 15 cm below soil
    target_flood_depth_cm: float = 5.0      # Reflood to 5 cm ponded depth
    critical_soil_moisture_percent: float = 30.0
    critical_dry_depth_cm: float = -20.0
    still_well_datum_cm: float = 20.0


@dataclass
class AWDEvaluationResult:
    timestamp: datetime
    field_state: AWDState
    average_water_level_cm: float
    average_soil_moisture_percent: float
    recommended_command: IrrigationCommandType
    rationale: str
    active_node_count: int
    readings: List[NodeReading] = field(default_factory=list)


class ArvaCoreDecisionEngine:
    def __init__(self, config: Optional[AWDThresholdConfig] = None):
        self.config = config or AWDThresholdConfig()

    def evaluate_field(self, readings: List[NodeReading]) -> AWDEvaluationResult:
        now = datetime.now()
        valid_readings = [r for r in readings if r.is_valid_distance and r.is_valid_soil_moisture]

        if not valid_readings:
            return AWDEvaluationResult(
                timestamp=now,
                field_state=AWDState.SAFE_DRYING,
                average_water_level_cm=0.0,
                average_soil_moisture_percent=0.0,
                recommended_command=IrrigationCommandType.HOLD,
                rationale="Insufficient valid telemetry readings from field nodes.",
                active_node_count=0,
                readings=[]
            )

        avg_water_level = sum(r.water_level_cm for r in valid_readings) / len(valid_readings)
        avg_moisture = sum(r.soil_moisture_percent for r in valid_readings) / len(valid_readings)

        # Decision matrix based on thesis Chapter 3 AWD methodology
        if avg_water_level <= self.config.critical_dry_depth_cm or avg_moisture < self.config.critical_soil_moisture_percent:
            state = AWDState.CRITICAL_DRY
            command = IrrigationCommandType.REFLOOD
            rationale = (
                f"Critical drying reached: Water level ({avg_water_level:.1f} cm) <= {self.config.critical_dry_depth_cm} cm "
                f"or soil moisture ({avg_moisture:.1f}%) < {self.config.critical_soil_moisture_percent}%. Urgent reflooding triggered."
            )
        elif avg_water_level <= self.config.reflood_trigger_depth_cm:
            state = AWDState.REFLOOD_REQUIRED
            command = IrrigationCommandType.REFLOOD
            rationale = (
                f"AWD reflood threshold reached: Water level ({avg_water_level:.1f} cm) reached "
                f"{self.config.reflood_trigger_depth_cm} cm. Reflooding field to {self.config.target_flood_depth_cm} cm."
            )
        elif avg_water_level >= self.config.target_flood_depth_cm:
            state = AWDState.FLOODED_OPTIMAL
            command = IrrigationCommandType.HOLD
            rationale = (
                f"Optimal flood level attained: Water level ({avg_water_level:.1f} cm) >= {self.config.target_flood_depth_cm} cm. "
                "Pump is held inactive while drying naturally."
            )
        else:
            state = AWDState.SAFE_DRYING
            command = IrrigationCommandType.HOLD
            rationale = (
                f"Field in safe AWD drying window ({avg_water_level:.1f} cm, {avg_moisture:.1f}% moisture). "
                "No irrigation needed."
            )

        return AWDEvaluationResult(
            timestamp=now,
            field_state=state,
            average_water_level_cm=round(avg_water_level, 2),
            average_soil_moisture_percent=round(avg_moisture, 2),
            recommended_command=command,
            rationale=rationale,
            active_node_count=len(valid_readings),
            readings=valid_readings
        )


class FlowVerificationSubsystem:
    """
    Evaluates pump execution and water delivery verification using the flow sensor.
    Distinguishes pump activation from physical water flow as mandated by thesis Section 3.
    """
    def __init__(self, min_verified_flow_rate_lpm: float = 5.0):
        self.min_flow_rate = min_verified_flow_rate_lpm

    def verify_irrigation_delivery(self, pump_active: bool, measured_flow_rate_lpm: float) -> Dict[str, Any]:
        if not pump_active:
            if measured_flow_rate_lpm > 0.5:
                return {
                    "verified": False,
                    "anomaly": "LEAK_OR_UNINTENDED_FLOW",
                    "message": f"Pump is OFF but flow sensor measures {measured_flow_rate_lpm:.1f} L/min."
                }
            return {
                "verified": True,
                "anomaly": None,
                "message": "System idle: pump inactive and flow line clear."
            }

        # Pump is ON
        if measured_flow_rate_lpm < self.min_flow_rate:
            return {
                "verified": False,
                "anomaly": "PUMP_DRY_RUN_OR_BLOCKAGE",
                "message": f"Pump is ON but flow rate ({measured_flow_rate_lpm:.1f} L/min) below threshold ({self.min_flow_rate} L/min)."
            }

        return {
            "verified": True,
            "anomaly": None,
            "message": f"Verified delivery: Pump is ON and flow confirmed at {measured_flow_rate_lpm:.1f} L/min."
        }

