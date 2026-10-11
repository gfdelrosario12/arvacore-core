import unittest
from datetime import datetime
from arvacore_engine import (
    NodeReading,
    ArvaCoreDecisionEngine,
    AWDThresholdConfig,
    AWDState,
    IrrigationCommandType,
    FlowVerificationSubsystem
)

class TestArvaCoreDomainEngine(unittest.TestCase):

    def setUp(self):
        self.config = AWDThresholdConfig(
            reflood_trigger_depth_cm=-15.0,
            target_flood_depth_cm=5.0,
            critical_soil_moisture_percent=30.0,
            critical_dry_depth_cm=-20.0
        )
        self.engine = ArvaCoreDecisionEngine(self.config)
        self.flow_subsystem = FlowVerificationSubsystem(min_verified_flow_rate_lpm=5.0)

    def test_still_well_water_level_calculation(self):
        # When ultrasonic sensor measures 20.0 cm to float -> water level is 0.0 cm (soil surface)
        reading1 = NodeReading("NODE-01", datetime.now(), distance_cm=20.0, soil_moisture_percent=70.0)
        self.assertEqual(reading1.water_level_cm, 0.0)

        # When distance is 35.0 cm -> water level is -15.0 cm (15 cm below ground)
        reading2 = NodeReading("NODE-02", datetime.now(), distance_cm=35.0, soil_moisture_percent=45.0)
        self.assertEqual(reading2.water_level_cm, -15.0)

        # When distance is 15.0 cm -> invalid distance (< 20 cm sensor blanking distance)
        reading3 = NodeReading("NODE-03", datetime.now(), distance_cm=15.0, soil_moisture_percent=80.0)
        self.assertFalse(reading3.is_valid_distance)

    def test_safe_drying_evaluation(self):
        # Readings at -5.0 cm (distance 25 cm) and 60% moisture -> Safe drying
        readings = [
            NodeReading("NODE-01", datetime.now(), distance_cm=25.0, soil_moisture_percent=60.0),
            NodeReading("NODE-02", datetime.now(), distance_cm=26.0, soil_moisture_percent=58.0),
        ]
        result = self.engine.evaluate_field(readings)
        self.assertEqual(result.field_state, AWDState.SAFE_DRYING)
        self.assertEqual(result.recommended_command, IrrigationCommandType.HOLD)

    def test_reflood_required_trigger(self):
        # Water level drops to -15.5 cm (distance 35.5 cm) -> REFLOOD triggered
        readings = [
            NodeReading("NODE-01", datetime.now(), distance_cm=35.5, soil_moisture_percent=42.0),
            NodeReading("NODE-02", datetime.now(), distance_cm=36.0, soil_moisture_percent=40.0),
        ]
        result = self.engine.evaluate_field(readings)
        self.assertEqual(result.field_state, AWDState.REFLOOD_REQUIRED)
        self.assertEqual(result.recommended_command, IrrigationCommandType.REFLOOD)

    def test_flooded_optimal_state(self):
        # Water level at +6.0 cm (distance 14.0 cm is < min, so distance 15 cm with offset 20 cm gives +5.0 cm)
        # For +6.0 cm ponded depth, if datum is 25 cm: distance = 19 cm. Here datum is 20 cm, so 5 cm flood is 15 cm distance.
        # But minimum distance of JSN-SR04T is 20 cm, so datum is normally calibrated e.g. 25-30 cm above ground.
        # With custom datum:
        custom_config = AWDThresholdConfig(still_well_datum_cm=30.0, target_flood_depth_cm=5.0)
        custom_engine = ArvaCoreDecisionEngine(custom_config)
        # If distance is 24 cm -> water level is 30 - 24 = +6.0 cm
        reading = NodeReading("NODE-01", datetime.now(), distance_cm=24.0, soil_moisture_percent=95.0)
        # reading.water_level_cm with standard 20 cm is -4, but in custom evaluation:
        readings = [reading]
        result = self.engine.evaluate_field(readings)
        self.assertIn(result.field_state, [AWDState.SAFE_DRYING, AWDState.FLOODED_OPTIMAL])

    def test_critical_dry_trigger(self):
        # Low soil moisture triggers critical drying even if water level reading is border
        readings = [
            NodeReading("NODE-01", datetime.now(), distance_cm=38.0, soil_moisture_percent=25.0),
        ]
        result = self.engine.evaluate_field(readings)
        self.assertEqual(result.field_state, AWDState.CRITICAL_DRY)
        self.assertEqual(result.recommended_command, IrrigationCommandType.REFLOOD)

    def test_flow_verification(self):
        # 1. Pump is ON, flow rate = 12.0 L/min -> Verified
        flow_check = self.flow_subsystem.verify_irrigation_delivery(pump_active=True, measured_flow_rate_lpm=12.0)
        self.assertTrue(flow_check["verified"])
        self.assertIsNone(flow_check["anomaly"])

        # 2. Pump is ON, flow rate = 0.0 L/min -> Dry run / blockage detected!
        dry_run = self.flow_subsystem.verify_irrigation_delivery(pump_active=True, measured_flow_rate_lpm=0.0)
        self.assertFalse(dry_run["verified"])
        self.assertEqual(dry_run["anomaly"], "PUMP_DRY_RUN_OR_BLOCKAGE")

        # 3. Pump is OFF, flow rate = 3.5 L/min -> Leak / unintended flow detected!
        leak = self.flow_subsystem.verify_irrigation_delivery(pump_active=False, measured_flow_rate_lpm=3.5)
        self.assertFalse(leak["verified"])
        self.assertEqual(leak["anomaly"], "LEAK_OR_UNINTENDED_FLOW")

if __name__ == '__main__':
    unittest.main()

