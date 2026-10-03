import re
from pathlib import Path

# Prints SG_ lines for these BO_ messages: name, [min|max], unit.
DBC = Path(__file__).resolve().parents[3] / "photon-next" / "assets" / "dbc" / "carcan_telemcan.dbc"
TEXT = DBC.read_text(encoding="utf-8", errors="ignore")
TARGETS = [
    "BPS_Status", "MC_Status", "MC_BusMeasurement", "Supp_Battery_Status",
    "Coolant_Temperature", "Pump_Status", "ChargerInterface_Status", "VCU_Status",
    "MC_Motor_TempMeasurement", "Telemetry_Status", "LV_Carrier_Status", "BPS_Fault_Val_Arr",
]

for block in re.split(r"\n(?=BO_ )", TEXT):
    m = re.match(r"BO_ \d+ (\w+):", block)
    if m and m.group(1) in TARGETS:
        print("##", m.group(1))
        for name, rng, unit in re.findall(r"SG_ (\w+) [^\n]*?\[([^\]]*)\] \"([^\"]*)\"", block):
            print(f"    {name} [{rng}] {unit}")
