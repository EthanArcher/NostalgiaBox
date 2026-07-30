import { getOpenAI } from "./openai";
import { encodeWav } from "../wav";

// The device plays audio at 16 kHz. Sending 16 kHz (instead of 24 kHz) is ~33%
// less data over Wi-Fi, so playback downloads faster and streams without gaps.
const OUTPUT_RATE = 16000;

/**
 * Downsample 24 kHz mono 16-bit PCM to 16 kHz using linear interpolation. Speech
 * quality at 16 kHz is plenty for the box, and the smaller payload is what keeps
 * playback smooth on marginal Wi-Fi.
 */
function downsample24to16(pcm24: Buffer): Buffer {
  const inSamples = pcm24.length >> 1;
  const outSamples = Math.floor((inSamples * OUTPUT_RATE) / 24000);
  const out = Buffer.alloc(outSamples * 2);
  for (let i = 0; i < outSamples; i++) {
    const srcPos = (i * 24000) / OUTPUT_RATE; // = i * 1.5
    const i0 = Math.floor(srcPos);
    const frac = srcPos - i0;
    const s0 = pcm24.readInt16LE(i0 * 2);
    const s1 = i0 + 1 < inSamples ? pcm24.readInt16LE((i0 + 1) * 2) : s0;
    let v = Math.round(s0 + (s1 - s0) * frac);
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    out.writeInt16LE(v, i * 2);
  }
  return out;
}

/**
 * Text-to-speech. Returns a 16 kHz mono 16-bit WAV (what the device expects).
 *
 * Provider is swappable via TTS_PROVIDER ("openai" | "elevenlabs") — a one-line
 * env change. Voice defaults to OpenAI's warm "nova".
 */
export async function synthesize(text: string): Promise<Buffer> {
  const provider = (process.env.TTS_PROVIDER || "openai").toLowerCase();
  if (provider === "elevenlabs") return synthesizeElevenLabs(text);
  return synthesizeOpenAI(text);
}

async function synthesizeOpenAI(text: string): Promise<Buffer> {
  const openai = getOpenAI();
  // Default to tts-1: it has much lower latency than gpt-4o-mini-tts, which makes
  // the box feel noticeably snappier. "nova" is warm on its own. Set TTS_MODEL to
  // "gpt-4o-mini-tts" if you want the steerable (but slower) voice.
  const model = process.env.TTS_MODEL || "tts-1";
  const voice = process.env.TTS_VOICE || "nova";

  const params: Parameters<typeof openai.audio.speech.create>[0] = {
    model,
    voice,
    input: text,
    response_format: "pcm", // raw 24kHz mono 16-bit LE
  };
  // Only the steerable gpt-4o TTS models accept `instructions`; tts-1 rejects it.
  if (model.includes("gpt-4o")) {
    params.instructions =
      "Speak in a warm, gentle, friendly female voice, slowly and clearly, as if reading to a young child.";
  }

  const res = await openai.audio.speech.create(params);
  const pcm24 = Buffer.from(await res.arrayBuffer()); // OpenAI pcm is 24 kHz
  const pcm16 = downsample24to16(pcm24);
  return encodeWav(pcm16, { sampleRate: OUTPUT_RATE, numChannels: 1 });
}

async function synthesizeElevenLabs(text: string): Promise<Buffer> {
  const key = process.env.ELEVENLABS_API_KEY;
  if (!key) throw new Error("ELEVENLABS_API_KEY is not set");
  const voiceId = process.env.ELEVENLABS_VOICE_ID || "EXAVITQu4vr4xnSDxMaL"; // warm default
  const modelId = process.env.ELEVENLABS_MODEL_ID || "eleven_flash_v2_5";

  const res = await fetch(
    `https://api.elevenlabs.io/v1/text-to-speech/${voiceId}?output_format=pcm_16000`,
    {
      method: "POST",
      headers: { "xi-api-key": key, "content-type": "application/json" },
      body: JSON.stringify({ text, model_id: modelId }),
    },
  );
  if (!res.ok) throw new Error(`ElevenLabs TTS failed: ${res.status}`);

  const pcm = Buffer.from(await res.arrayBuffer()); // requested at 16 kHz
  return encodeWav(pcm, { sampleRate: OUTPUT_RATE, numChannels: 1 });
}
