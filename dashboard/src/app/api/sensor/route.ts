export const dynamic = "force-dynamic";

export type VesselStatus = "SAFE" | "ILLEGAL VESSEL DETECTED";

export interface SensorData {
  spl: number | null;
  fft: number | null;
  ema: number | null;
  lat: number | null;
  lon: number | null;
  sat: number | null;
  pdr: number;
  rssi: number | null;
  snr: number | null;
  status: VesselStatus | null;
  updatedAt: number | null;
}

// In-memory store for ESP32 POST payloads (resets on server restart)
let latestData: SensorData = {
  spl: null,
  fft: null,
  ema: null,
  lat: null,
  lon: null,
  sat: null,
  pdr: 0,
  rssi: null,
  snr: null,
  status: null,
  updatedAt: null,
};

function computeStatus(spl: number, fft: number, ema: number): VesselStatus {
  if (spl > 70 && fft > 5 && ema > 65) {
    return "ILLEGAL VESSEL DETECTED";
  }
  return "SAFE";
}

function isNumber(value: unknown): value is number {
  return typeof value === "number" && !Number.isNaN(value);
}

export async function POST(request: Request) {
  try {
    const body = await request.json();

    const requiredFields = ["spl", "fft", "ema", "lat", "lon", "sat", "pdr", "rssi", "snr"] as const;
    const missing = requiredFields.filter((key) => !isNumber(body[key]));

    if (missing.length > 0) {
      return Response.json(
        {
          error: `Expected numeric fields: ${requiredFields.join(", ")}`,
          missing,
        },
        { status: 400 }
      );
    }

    latestData = {
      spl: body.spl,
      fft: body.fft,
      ema: body.ema,
      lat: body.lat,
      lon: body.lon,
      sat: body.sat,
      pdr: body.pdr,
      rssi: body.rssi,
      snr: body.snr,
      status: computeStatus(body.spl, body.fft, body.ema),
      updatedAt: Date.now(),
    };
    console.log("DATA MASUK:", latestData);

    return Response.json({ success: true, data: latestData });
  } catch {
    return Response.json({ error: "Invalid JSON body" }, { status: 400 });
  }
}

export async function GET() {
  return Response.json(latestData, {
    headers: {
      "Cache-Control": "no-store, no-cache",
    },
  });
}
