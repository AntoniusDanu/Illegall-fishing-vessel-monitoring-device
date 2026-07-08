"use client";

import { useEffect, useMemo, useRef, useState } from "react";
import {
  Activity,
  AlertCircle,
  Anchor,
  Loader2,
  MapPin,
  Radio,
  Signal,
  Volume2,
  Waves,
  Wifi,
} from "lucide-react";
import {
  Bar,
  BarChart,
  CartesianGrid,
  Cell,
  Legend,
  Line,
  LineChart,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";
import { cn } from "@/lib/utils";

// ——— Types & constants ———

type MonitoringStatus = "SAFE" | "ILLEGAL VESSEL DETECTED";

interface SplPoint {
  time: string;
  spl: number;
  ema: number;
}

interface LoraPoint {
  time: string;
  rssi: number;
  snr: number;
}

interface FftBand {
  frequency: string;
  energy: number;
  hz: number;
}

const FFT_FREQUENCIES = [50, 100, 150, 200, 250, 300, 350, 400, 450, 500];
const FFT_ENERGY_THRESHOLD = 72;
const MAX_HISTORY = 36;

interface SensorApiResponse {
  spl: number | null;
  fft: number | null;
  ema: number | null;
  lat: number | null;
  lon: number | null;
  sat: number | null;
  pdr: number | null;
  rssi: number | null;
  snr: number | null;
  status: MonitoringStatus | null;
  updatedAt: number | null;
}

const POLL_INTERVAL_MS = 2000;

// ——— Helpers ———

function formatTime(date: Date): string {
  return date.toLocaleTimeString("id-ID", {
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
  });
}

function statusVariant(
  status: MonitoringStatus
): "safe" | "warning" | "danger" {
  return status === "ILLEGAL VESSEL DETECTED" ? "danger" : "safe";
}

function formatValue(value: number | null, decimals = 1): string {
  return value === null ? "—" : value.toFixed(decimals);
}

function buildFftBands(fftEnergy: number): FftBand[] {
  return FFT_FREQUENCIES.map((hz) => ({
    frequency: `${hz}Hz`,
    energy: Math.round(fftEnergy * 10) / 10,
    hz,
  }));
}

function hasSensorData(data: SensorApiResponse): boolean {
  return data.updatedAt !== null;
}

// ——— Sensor polling component ———

function SensorMonitor({
  onData,
  onLoadingChange,
  onError,
}: {
  onData: (data: SensorApiResponse) => void;
  onLoadingChange: (loading: boolean) => void;
  onError: (error: string | null) => void;
}) {
  useEffect(() => {
    let isMounted = true;

    const fetchSensorData = async () => {
      try {
        const response = await fetch("/api/sensor", {
          cache: "no-store",
          headers: {
            "Bypass-Tunnel-Reminder": "true",
            "ngrok-skip-browser-warning": "true",
          },
        });

        if (!response.ok) {
          throw new Error(`Gagal memuat data (HTTP ${response.status})`);
        }

        const data: SensorApiResponse = await response.json();

        if (!isMounted) return;

        onError(null);
        onData(data);
      } catch (err) {
        if (!isMounted) return;
        onError(
          err instanceof Error ? err.message : "Terjadi kesalahan jaringan"
        );
      } finally {
        if (isMounted) {
          onLoadingChange(false);
        }
      }
    };

    onLoadingChange(true);
    fetchSensorData();
    const interval = setInterval(fetchSensorData, POLL_INTERVAL_MS);

    return () => {
      isMounted = false;
      clearInterval(interval);
    };
  }, [onData, onLoadingChange, onError]);

  return null;
}

// ——— Sub-components ———

function SummaryCard({
  title,
  value,
  unit,
  icon: Icon,
  accent = "cyan",
}: {
  title: string;
  value: string | number;
  unit: string;
  icon: React.ComponentType<{ className?: string }>;
  accent?: "cyan" | "emerald" | "violet" | "amber";
}) {
  const accentMap = {
    cyan: "text-cyan-400 bg-cyan-500/10 border-cyan-500/20",
    emerald: "text-emerald-400 bg-emerald-500/10 border-emerald-500/20",
    violet: "text-violet-400 bg-violet-500/10 border-violet-500/20",
    amber: "text-amber-400 bg-amber-500/10 border-amber-500/20",
  };

  return (
    <Card className="overflow-hidden">
      <CardContent className="p-5">
        <div className="flex items-start justify-between">
          <div>
            <p className="text-xs font-medium uppercase tracking-wider text-slate-400">
              {title}
            </p>
            <p className="mt-2 font-mono text-3xl font-bold text-white tabular-nums">
              {value}
              <span className="ml-1 text-lg font-normal text-slate-400">
                {unit}
              </span>
            </p>
          </div>
          <div
            className={cn(
              "rounded-lg border p-2.5",
              accentMap[accent]
            )}
          >
            <Icon className="h-5 w-5" />
          </div>
        </div>
      </CardContent>
    </Card>
  );
}

const chartTooltipStyle = {
  contentStyle: {
    background: "rgba(15, 23, 42, 0.95)",
    border: "1px solid rgba(6, 182, 212, 0.3)",
    borderRadius: "8px",
    fontSize: "12px",
  },
  labelStyle: { color: "#94a3b8" },
};

// ——— Main page ———

export default function DashboardPage() {
  const lastUpdatedAtRef = useRef<number | null>(null);

  const [isLoading, setIsLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [status, setStatus] = useState<MonitoringStatus>("SAFE");
  const [spl, setSpl] = useState<number | null>(null);
  const [fft, setFft] = useState<number | null>(null);
  const [ema, setEma] = useState<number | null>(null);
  const [lat, setLat] = useState<number | null>(null);
  const [lon, setLon] = useState<number | null>(null);
  const [sat, setSat] = useState<number | null>(null);
  const [pdr, setPdr] = useState<number | null>(null);
  const [rssi, setRssi] = useState<number | null>(null);
  const [snr, setSnr] = useState<number | null>(null);
  const [splHistory, setSplHistory] = useState<SplPoint[]>([]);
  const [fftBands, setFftBands] = useState<FftBand[]>([]);
  const [loraHistory, setLoraHistory] = useState<LoraPoint[]>([]);
  const [lastUpdate, setLastUpdate] = useState<string>("—");
  const [hasData, setHasData] = useState(false);

  const handleSensorData = useMemo(
    () => (data: SensorApiResponse) => {
      const received = hasSensorData(data);
      setHasData(received);

      if (!received) return;

      setSpl(data.spl !== null ? Math.round(data.spl * 10) / 10 : null);
      setFft(data.fft !== null ? Math.round(data.fft * 10) / 10 : null);
      setEma(data.ema !== null ? Math.round(data.ema * 10) / 10 : null);
      setLat(data.lat);
      setLon(data.lon);
      setSat(data.sat);
      setPdr(data.pdr !== null ? Math.round(data.pdr * 10) / 10 : null);
      setRssi(data.rssi);
      setSnr(data.snr);
      setStatus(data.status ?? "SAFE");

      if (data.fft !== null) {
        setFftBands(buildFftBands(data.fft));
      }

      if (data.updatedAt !== null && data.updatedAt !== lastUpdatedAtRef.current) {
        lastUpdatedAtRef.current = data.updatedAt;
        const timeLabel = formatTime(new Date(data.updatedAt));
        setLastUpdate(timeLabel);

        if (data.spl !== null && data.ema !== null) {
          setSplHistory((prev) => {
            const next = [
              ...prev,
              { time: timeLabel, spl: data.spl!, ema: data.ema! },
            ];
            return next.slice(-MAX_HISTORY);
          });
        }

        if (data.rssi !== null && data.snr !== null) {
          setLoraHistory((prev) => {
            const next = [
              ...prev,
              { time: timeLabel, rssi: data.rssi!, snr: data.snr! },
            ];
            return next.slice(-MAX_HISTORY);
          });
        }
      }
    },
    []
  );

  const handleLoadingChange = useMemo(
    () => (loading: boolean) => setIsLoading(loading),
    []
  );

  const handleError = useMemo(
    () => (message: string | null) => setError(message),
    []
  );

  const statusIcon = useMemo(() => {
    switch (status) {
      case "SAFE":
        return <Anchor className="h-4 w-4" />;
      case "ILLEGAL VESSEL DETECTED":
        return <Waves className="h-4 w-4" />;
    }
  }, [status]);

  return (
    <div className="dashboard-bg min-h-screen">
      <SensorMonitor
        onData={handleSensorData}
        onLoadingChange={handleLoadingChange}
        onError={handleError}
      />

      <div className="mx-auto max-w-[1600px] space-y-6 p-4 md:p-6 lg:p-8">
        {/* ——— LOADING / ERROR BANNERS ——— */}
        {isLoading && (
          <div className="flex items-center gap-3 rounded-lg border border-cyan-500/30 bg-cyan-500/10 px-4 py-3 text-sm text-cyan-200">
            <Loader2 className="h-4 w-4 shrink-0 animate-spin" />
            Memuat data sensor...
          </div>
        )}

        {error && (
          <div className="flex items-center gap-3 rounded-lg border border-red-500/40 bg-red-500/10 px-4 py-3 text-sm text-red-200">
            <AlertCircle className="h-4 w-4 shrink-0" />
            {error}
          </div>
        )}

        {!isLoading && !error && !hasData && (
          <div className="flex items-center gap-3 rounded-lg border border-amber-500/30 bg-amber-500/10 px-4 py-3 text-sm text-amber-200">
            <Radio className="h-4 w-4 shrink-0" />
            Menunggu data pertama dari ESP32 via POST /api/sensor...
          </div>
        )}

        {/* ——— TOP HEADER ——— */}
        <header className="flex flex-col gap-4 border-b border-cyan-900/30 pb-6 lg:flex-row lg:items-center lg:justify-between">
          <div className="flex items-start gap-4">
            <div className="hidden rounded-xl border border-cyan-500/30 bg-cyan-500/10 p-3 sm:block">
              <Radio className="h-8 w-8 text-cyan-400" />
            </div>
            <div>
              <p className="text-xs font-medium uppercase tracking-[0.2em] text-cyan-500/80">
                Smart Buoy · Maritime Acoustic Monitoring
              </p>
              <h1 className="mt-1 max-w-3xl text-xl font-bold leading-tight text-white md:text-2xl lg:text-[1.65rem]">
                Sistem Pemantauan Akustik Illegal Fishing (LoRa & ESP32)
              </h1>
              <p className="mt-1 font-mono text-xs text-slate-500">
                Pembaruan terakhir: {lastUpdate} · Polling {POLL_INTERVAL_MS / 1000}s
              </p>
            </div>
          </div>

          <div className="flex flex-wrap items-center gap-3 lg:justify-end">
            <Badge
              variant={statusVariant(status)}
              className="gap-2 px-5 py-2 text-sm"
            >
              {statusIcon}
              {status}
            </Badge>
          </div>
        </header>

        {/* ——— LIVE SENSOR METRICS ——— */}
        <section className="grid grid-cols-2 gap-3 sm:grid-cols-4 xl:grid-cols-8">
          <SummaryCard
            title="SPL"
            value={formatValue(spl)}
            unit="dB"
            icon={Volume2}
            accent="cyan"
          />
          <SummaryCard
            title="FFT"
            value={formatValue(fft)}
            unit=""
            icon={Activity}
            accent="violet"
          />
          <SummaryCard
            title="EMA"
            value={formatValue(ema)}
            unit="dB"
            icon={Waves}
            accent="emerald"
          />
          <SummaryCard
            title="Latitude"
            value={lat !== null ? lat.toFixed(5) : "—"}
            unit="°"
            icon={MapPin}
            accent="cyan"
          />
          <SummaryCard
            title="Longitude"
            value={lon !== null ? lon.toFixed(5) : "—"}
            unit="°"
            icon={MapPin}
            accent="cyan"
          />
          <SummaryCard
            title="PDR"
            value={formatValue(pdr)}
            unit="%"
            icon={Radio}
            accent="amber"
          />
          <SummaryCard
            title="RSSI"
            value={formatValue(rssi)}
            unit="dBm"
            icon={Signal}
            accent="violet"
          />
          <SummaryCard
            title="SNR"
            value={formatValue(snr)}
            unit="dB"
            icon={Wifi}
            accent="emerald"
          />
        </section>

        {/* ——— MAIN CHARTS ——— */}
        <section className="grid grid-cols-1 gap-6 xl:grid-cols-2">
          <Card>
            <CardHeader>
              <CardTitle className="flex items-center gap-2">
                <Volume2 className="h-4 w-4 text-cyan-400" />
                SPL & Pola Temporal
              </CardTitle>
              <p className="text-xs text-slate-500">
                Garis SPL (dB) dan EMA — indikasi peningkatan suara mesin kapal
              </p>
            </CardHeader>
            <CardContent>
              <div className="h-[320px] w-full">
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart
                    data={splHistory}
                    margin={{ top: 8, right: 12, left: 0, bottom: 0 }}
                  >
                    <CartesianGrid
                      strokeDasharray="3 3"
                      stroke="rgba(6, 182, 212, 0.1)"
                    />
                    <XAxis
                      dataKey="time"
                      tick={{ fill: "#64748b", fontSize: 10 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(6, 182, 212, 0.2)" }}
                      interval="preserveStartEnd"
                    />
                    <YAxis
                      domain={["auto", "auto"]}
                      tick={{ fill: "#64748b", fontSize: 11 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(6, 182, 212, 0.2)" }}
                      label={{
                        value: "dB",
                        angle: -90,
                        position: "insideLeft",
                        fill: "#64748b",
                        fontSize: 11,
                      }}
                    />
                    <Tooltip {...chartTooltipStyle} />
                    <Legend
                      wrapperStyle={{ fontSize: "12px", paddingTop: "8px" }}
                    />
                    <Line
                      type="monotone"
                      dataKey="spl"
                      name="SPL (dB)"
                      stroke="#22d3ee"
                      strokeWidth={2}
                      dot={false}
                      activeDot={{ r: 4, fill: "#22d3ee" }}
                    />
                    <Line
                      type="monotone"
                      dataKey="ema"
                      name="EMA"
                      stroke="#a78bfa"
                      strokeWidth={2}
                      strokeDasharray="6 4"
                      dot={false}
                    />
                  </LineChart>
                </ResponsiveContainer>
              </div>
            </CardContent>
          </Card>

          <Card>
            <CardHeader>
              <CardTitle className="flex items-center gap-2">
                <Activity className="h-4 w-4 text-cyan-400" />
                FFT Frequency Energy
              </CardTitle>
              <p className="text-xs text-slate-500">
                Wide-band 50–2000 Hz · deteksi signature mesin/propeller hingga
                2 kHz · merah jika energi &gt; {FFT_ENERGY_THRESHOLD}
              </p>
            </CardHeader>
            <CardContent>
              <div className="h-[320px] w-full">
                <ResponsiveContainer width="100%" height="100%">
                  <BarChart
                    data={fftBands}
                    margin={{ top: 8, right: 12, left: 0, bottom: 0 }}
                  >
                    <CartesianGrid
                      strokeDasharray="3 3"
                      stroke="rgba(6, 182, 212, 0.1)"
                      vertical={false}
                    />
                    <XAxis
                      dataKey="frequency"
                      tick={{ fill: "#64748b", fontSize: 10 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(6, 182, 212, 0.2)" }}
                    />
                    <YAxis
                      domain={["auto", "auto"]}
                      tick={{ fill: "#64748b", fontSize: 11 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(6, 182, 212, 0.2)" }}
                      label={{
                        value: "Energi",
                        angle: -90,
                        position: "insideLeft",
                        fill: "#64748b",
                        fontSize: 11,
                      }}
                    />
                    <Tooltip {...chartTooltipStyle} />
                    <Bar dataKey="energy" name="Energi" radius={[4, 4, 0, 0]}>
                      {fftBands.map((entry, index) => (
                        <Cell
                          key={`cell-${index}`}
                          fill={
                            entry.energy > FFT_ENERGY_THRESHOLD
                              ? "#ef4444"
                              : "#0891b2"
                          }
                        />
                      ))}
                    </Bar>
                  </BarChart>
                </ResponsiveContainer>
              </div>
            </CardContent>
          </Card>
        </section>

        {/* ——— LORA HEALTH & LOCATION ——— */}
        <section className="grid grid-cols-1 gap-6 xl:grid-cols-2">
          <Card>
            <CardHeader>
              <CardTitle className="flex items-center gap-2">
                <Signal className="h-4 w-4 text-cyan-400" />
                LoRa Signal Quality
              </CardTitle>
              <p className="text-xs text-slate-500">
                RSSI & SNR over time — stabilitas transmisi
              </p>
            </CardHeader>
            <CardContent>
              <div className="h-[280px] w-full">
                <ResponsiveContainer width="100%" height="100%">
                  <LineChart
                    data={loraHistory}
                    margin={{ top: 8, right: 12, left: 0, bottom: 0 }}
                  >
                    <CartesianGrid
                      strokeDasharray="3 3"
                      stroke="rgba(6, 182, 212, 0.1)"
                    />
                    <XAxis
                      dataKey="time"
                      tick={{ fill: "#64748b", fontSize: 10 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(6, 182, 212, 0.2)" }}
                      interval="preserveStartEnd"
                    />
                    <YAxis
                      yAxisId="rssi"
                      domain={["auto", "auto"]}
                      tick={{ fill: "#a78bfa", fontSize: 11 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(167, 139, 250, 0.3)" }}
                      label={{
                        value: "dBm",
                        angle: -90,
                        position: "insideLeft",
                        fill: "#a78bfa",
                        fontSize: 10,
                      }}
                    />
                    <YAxis
                      yAxisId="snr"
                      orientation="right"
                      domain={["auto", "auto"]}
                      tick={{ fill: "#34d399", fontSize: 11 }}
                      tickLine={false}
                      axisLine={{ stroke: "rgba(52, 211, 153, 0.3)" }}
                      label={{
                        value: "SNR dB",
                        angle: 90,
                        position: "insideRight",
                        fill: "#34d399",
                        fontSize: 10,
                      }}
                    />
                    <Tooltip {...chartTooltipStyle} />
                    <Legend wrapperStyle={{ fontSize: "12px" }} />
                    <Line
                      yAxisId="rssi"
                      type="monotone"
                      dataKey="rssi"
                      name="RSSI (dBm)"
                      stroke="#a78bfa"
                      strokeWidth={2}
                      dot={false}
                    />
                    <Line
                      yAxisId="snr"
                      type="monotone"
                      dataKey="snr"
                      name="SNR (dB)"
                      stroke="#34d399"
                      strokeWidth={2}
                      dot={false}
                    />
                  </LineChart>
                </ResponsiveContainer>
              </div>
            </CardContent>
          </Card>

          <Card className="overflow-hidden">
            <CardHeader>
              <CardTitle className="flex items-center gap-2">
                <MapPin className="h-4 w-4 text-cyan-400" />
                GPS Location
              </CardTitle>
              <p className="text-xs text-slate-500">
                Placeholder peta — react-leaflet akan ditambahkan
              </p>
            </CardHeader>
            <CardContent>
              <div className="relative flex h-[280px] flex-col items-center justify-center overflow-hidden rounded-lg border border-dashed border-cyan-700/40 bg-gradient-to-br from-slate-900 via-slate-950 to-cyan-950/30">
                <div
                  className="pointer-events-none absolute inset-0 opacity-30"
                  style={{
                    backgroundImage: `
                      linear-gradient(rgba(6,182,212,0.15) 1px, transparent 1px),
                      linear-gradient(90deg, rgba(6,182,212,0.15) 1px, transparent 1px)
                    `,
                    backgroundSize: "24px 24px",
                  }}
                />
                <div className="relative z-10 flex flex-col items-center gap-4 text-center">
                  <div className="rounded-full border border-cyan-500/40 bg-cyan-500/10 p-4">
                    <MapPin className="h-10 w-10 text-cyan-400" />
                  </div>
                  <div>
                    <p className="text-sm font-medium text-slate-300">
                      Koordinat Pelampung
                    </p>
                    <p className="mt-2 font-mono text-lg text-cyan-300">
                      Lat: {formatValue(lat, 5)}, Lng: {formatValue(lon, 5)}
                    </p>
                    <p className="mt-1 font-mono text-sm text-slate-400">
                      Satelit GPS: {sat !== null ? sat : "—"}
                    </p>
                    <p className="mt-1 text-xs text-slate-500">
                      Wilayah pemantauan perairan · Smart Buoy #01
                    </p>
                  </div>
                </div>
              </div>
            </CardContent>
          </Card>
        </section>

        <footer className="border-t border-cyan-900/20 pt-4 text-center text-xs text-slate-600">
          Dashboard Skripsi · Smart Buoy Acoustic Monitoring · ESP32 live data
        </footer>
      </div>
    </div>
  );
}
