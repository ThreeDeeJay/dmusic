// Copyright © 2024. GothicKit Contributors
// SPDX-License-Identifier: MIT-Modern-Variant
#include "_Internal.h"
#include <math.h>

#include <tsf.h>

static double dlsTimeCentsToSeconds(int32_t tc) {
	return exp2((double) tc / (1200.0 * 65536.0));
}

static int16_t sf2SecondsToTimeCents(double secs) {
	return (int16_t) (1200.0 * log2(secs));
}

enum {
	kPan = 17,
	kAttackModEnv = 26,
	kDecayModEnv = 28,
	kSustainModEnv = 29,
	kReleaseModEnv = 30,
	kAttackVolEnv = 34,
	kDecayVolEnv = 36,
	kSustainVolEnv = 37,
	kReleaseVolEnv = 38,
	kInstrument = 41,
	kKeyRange = 43,
	kVelRange = 44,
	kInitialAttenuation = 48,
	kSampleID = 53,
	kSampleModes = 54,

	kSamplePadding = 46,
};

static DmDlsInstrument* DmSynth_findDlsInstrument(DmInstrument* slf) {
	if (slf->flags & (DmInstrument_GM | DmInstrument_GS)) {
		Dm_report(DmLogLevel_DEBUG, "DmSynth: Not loading GM instrument");
		return NULL;
	}

	if (slf->dls == NULL) {
		Dm_report(DmLogLevel_ERROR, "DmSynth: Cannot resolve DLS instrument: DLS not loaded");
		return NULL;
	}

	if (!(slf->flags & DmInstrument_PATCH)) {
		Dm_report(DmLogLevel_ERROR, "DmSynth: Cannot resolve DLS instrument: Instrument patch not valid");
		return NULL;
	}

	uint32_t bank = (slf->patch & 0xFF00U) >> 8;
	uint32_t patch = slf->patch & 0xFFU;
	DmDlsInstrument* dls = NULL;
	for (size_t i = 0; i < slf->dls->instrument_count; ++i) {
		dls = &slf->dls->instruments[i];

		// TODO(lmichaelis): We need to ignore drum kits for now since I don't know how to handle them properly
		if (dls->bank & (1U << 31U)) {
			continue;
		}

		// If it's the correct instrument, return it.
		if (dls->bank == bank && dls->patch == patch) {
			return dls;
		}
	}

	Dm_report(DmLogLevel_ERROR, "DmSynth: Cannot resolve DLS instrument: Patch %d:%d not found", bank, patch);
	return NULL;
}

static size_t DmSynth_convertGeneratorArticulators(struct tsf_hydra_igen* gens, DmDlsArticulator* art) {
	size_t count = 0;

	for (size_t k = 0; k < art->connection_count; ++k) {
		struct DmDlsArticulatorConnection* con = &art->connections[k];
		struct tsf_hydra_igen* gen = &gens[count];

		bool is_modulator = con->source != DmDlsArticulatorSource_NONE ||
		    con->transform != DmDlsArticulatorTransform_NONE || con->control != 0;
		if (is_modulator) {
			Dm_report(DmLogLevel_WARN, "DmSynth: DLS Modulators are not supported");
			continue;
		}

		switch (con->destination) {
		case DmDlsArticulatorDestination_PAN:
			gen->genOper = kPan;
			gen->genAmount.shortAmount = (tsf_s16) (con->scale / 65535);
			break;
		case DmDlsArticulatorDestination_EG1_ATTACK_TIME:
			gen->genOper = kAttackVolEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG2_ATTACK_TIME:
			gen->genOper = kAttackModEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG1_DECAY_TIME:
			gen->genOper = kDecayVolEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG2_DECAY_TIME:
			gen->genOper = kDecayModEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG1_RELEASE_TIME:
			gen->genOper = kReleaseVolEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG2_RELEASE_TIME:
			gen->genOper = kReleaseModEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG1_SUSTAIN_LEVEL:
			// FIXME: This is not correct! The DLS value is in 0.1% and the SF2 value is in "centibels"
			gen->genOper = kSustainVolEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		case DmDlsArticulatorDestination_EG2_SUSTAIN_LEVEL:
			// FIXME: This is not correct! The DLS value is in 0.1% and the SF2 value is in "centibels"
			gen->genOper = kSustainModEnv;
			gen->genAmount.shortAmount = sf2SecondsToTimeCents(dlsTimeCentsToSeconds(con->scale));
			break;
		default:
			Dm_report(DmLogLevel_WARN, "DmSynth: Unknown Instrument Generator: %d", con->destination);
			continue;
		}

		count += 1;
	}

	return count;
}

DmResult DmSynth_createTsfForInstrument(DmInstrument* slf, tsf** out) {
	DmDlsInstrument* dls = DmSynth_findDlsInstrument(slf);
	if (dls == NULL) {
		return DmResult_NOT_FOUND;
	}

	// 1. Decode the samples for each region into a contiguous array and create the SF2
	//    sample headers for them.

	// 1.1. Count the number of PCM samples actually required after decoding.
	size_t sample_count = 0;
	for (uint32_t i = 0; i < dls->region_count; ++i) {
		DmDlsWave* wav = &slf->dls->wave_table[dls->regions[i].link_table_index];
		sample_count += DmDls_decodeSamples(wav, NULL, 0);

		// There are 46 0-samples after each "real" sample
		sample_count += kSamplePadding;
	}

	// 1.2. Decode all required samples
	size_t sample_headers_length = dls->region_count + 1; // One for the sentinel
	struct tsf_hydra_shdr* sample_headers = Dm_alloc(sizeof(struct tsf_hydra_shdr) * sample_headers_length);
	if (sample_headers == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	float* samples = Dm_alloc(sizeof(float) * sample_count);
	if (samples == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	uint32_t sample_offset = 0;
	for (uint32_t i = 0; i < dls->region_count; ++i) {
		DmDlsWave* wav = &slf->dls->wave_table[dls->regions[i].link_table_index];

		strncpy(sample_headers[i].sampleName, wav->info.inam, 19);

		sample_headers[i].start = sample_offset;
		sample_headers[i].sampleRate = wav->samples_per_second;
		sample_headers[i].sampleType = 1; // SFSampleLink::monoSample
		sample_offset += DmDls_decodeSamples(wav, samples + sample_offset, sample_count - sample_offset);
		sample_headers[i].end = sample_offset;

		// There are 46 0-samples after each "real" sample
		sample_offset += kSamplePadding;
	}

	strncpy(sample_headers[sample_headers_length - 1].sampleName, "EOS", 19);

	// 2. Generate the SF2 instrument zones from the DLS instrument regions.

	// 2.1. Count the number of SF2 instrument generators (and modulators) required
	// NOTE: One for the sentinel, 6 for the implicit generators 'kKeyRange',
	//       'kVelRange', 'kAttackVolEnv', 'kInitialAttenuation', 'kSampleModes' and 'kSampleID' for each zone
	size_t instrument_generator_count = 1 + (6 * dls->region_count);

	// NOTE: We only support generators at the moment.
	for (size_t i = 0; i < dls->region_count; ++i) {
		DmDlsRegion* region = &dls->regions[i];
		for (size_t j = 0; j < region->articulator_count; ++j) {
			DmDlsArticulator* articulator = &region->articulators[j];
			instrument_generator_count += articulator->connection_count;
		}
	}

	for (size_t j = 0; j < dls->articulator_count; ++j) {
		DmDlsArticulator* articulator = &dls->articulators[j];
		instrument_generator_count += articulator->connection_count * dls->region_count;
	}

	// 2.2. Actually create the instrument zone definitions
	size_t instrument_zones_length = dls->region_count + 1; // One for the sentinel
	struct tsf_hydra_ibag* instrument_zones = Dm_alloc(sizeof(struct tsf_hydra_shdr) * instrument_zones_length);
	struct tsf_hydra_igen* instrument_generators = Dm_alloc(sizeof(struct tsf_hydra_igen) * instrument_generator_count);
	struct tsf_hydra_imod instrument_modulators[1]; // Only one, indicating a sentinel, for now

	if (instrument_zones == NULL || instrument_generators == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	size_t generator_index = 0;
	for (size_t i = 0; i < dls->region_count; ++i) {
		DmDlsRegion* region = &dls->regions[i];

		// Instrument zone header.
		instrument_zones[i].instGenNdx = (tsf_u16) generator_index;
		instrument_zones[i].instModNdx = 0; // TODO(lmichaelis): Implement modulators.

		struct tsf_hydra_igen* gen;
		gen = &instrument_generators[generator_index++];
		gen->genOper = kKeyRange;
		gen->genAmount.range.hi = (tsf_u8) region->range_high;
		gen->genAmount.range.lo = (tsf_u8) region->range_low;

		gen = &instrument_generators[generator_index++];
		gen->genOper = kVelRange;

		if (region->velocity_high <= region->velocity_low) {
			gen->genAmount.range.hi = 127;
			gen->genAmount.range.lo = 0;
		} else {
			gen->genAmount.range.hi = (tsf_u8) region->velocity_high;
			gen->genAmount.range.lo = (tsf_u8) region->velocity_low;
		}

		gen = &instrument_generators[generator_index++];
		gen->genOper = kAttackVolEnv;
		gen->genAmount.shortAmount = sf2SecondsToTimeCents(0.1);

		gen = &instrument_generators[generator_index++];
		gen->genOper = kInitialAttenuation;
		gen->genAmount.shortAmount = (tsf_s16) region->sample.attenuation;

		for (size_t j = 0; j < region->articulator_count; ++j) {
			DmDlsArticulator* articulator = &region->articulators[j];

			gen = &instrument_generators[generator_index];
			generator_index += DmSynth_convertGeneratorArticulators(gen, articulator);
		}

		for (size_t j = 0; j < dls->articulator_count; ++j) {
			DmDlsArticulator* articulator = &dls->articulators[j];

			gen = &instrument_generators[generator_index];
			generator_index += DmSynth_convertGeneratorArticulators(gen, articulator);
		}

		gen = &instrument_generators[generator_index++];
		gen->genOper = kSampleModes;
		gen->genAmount.wordAmount = region->sample.looping == true ? 1 : 0;

		gen = &instrument_generators[generator_index++];
		gen->genOper = kSampleID;
		gen->genAmount.wordAmount = (tsf_u16) i;

		// Additional sample configuration.
		if (region->sample.looping) {
			uint32_t offset = sample_headers[i].start;
			sample_headers[i].startLoop = offset + region->sample.loop_start;
			sample_headers[i].endLoop = offset + region->sample.loop_start + region->sample.loop_length;

			// NOTE: Fix for sound cutting off too early. When using TSF.
			if (sample_headers[i].endLoop > sample_headers[i].end) {
				sample_headers[i].endLoop = sample_headers[i].end;
			}
		} else {
			sample_headers[i].startLoop = 0;
			sample_headers[i].endLoop = 0;
		}

		sample_headers[i].originalPitch = (tsf_u8) region->sample.unity_note;
		sample_headers[i].pitchCorrection = (tsf_s8) region->sample.fine_tune;
	}

	// 2.3. Make sure the sentinel values are correct
	instrument_zones[dls->region_count].instGenNdx = (tsf_u16) generator_index;
	instrument_zones[dls->region_count].instModNdx = 0;

	// 3. Generate preset-level articulators

	struct tsf_hydra_pbag preset_zones[2];      // One for the sentinel
	struct tsf_hydra_pgen preset_generators[2]; // One for the sentinel
	struct tsf_hydra_pmod preset_modulators[1]; // Only the sentinel

	preset_generators[0].genOper = kInstrument;
	preset_generators[0].genAmount.wordAmount = 0;
	generator_index += 1;

	preset_generators[1].genOper = 0;
	preset_generators[1].genAmount.wordAmount = 1;
	generator_index += 1;

	preset_zones[0].genNdx = 0;
	preset_zones[0].modNdx = 0;
	preset_zones[1].genNdx = (tsf_u16) generator_index;
	preset_zones[1].modNdx = 0;

	// 4. Finalize the instrument and preset
	struct tsf_hydra_phdr preset_headers[2]; // One for the sentinel

	strncpy(preset_headers[0].presetName, dls->info.inam, 19);
	strncpy(preset_headers[1].presetName, "EOP", 19);

	preset_headers[0].bank = 0;
	preset_headers[0].preset = 0;
	preset_headers[0].genre = 0;
	preset_headers[0].morphology = 0;
	preset_headers[0].library = 0;
	preset_headers[0].presetBagNdx = 0;

	preset_headers[1].bank = 0;
	preset_headers[1].preset = 0;
	preset_headers[1].genre = 0;
	preset_headers[1].morphology = 0;
	preset_headers[1].library = 0;
	preset_headers[1].presetBagNdx = 1;

	struct tsf_hydra_inst instrument_headers[2]; // One for the sentinel

	strncpy(instrument_headers[0].instName, dls->info.inam, 19);
	strncpy(instrument_headers[1].instName, "EOI", 19);

	instrument_headers[0].instBagNdx = 0;
	instrument_headers[1].instBagNdx = (tsf_u16) dls->region_count;

	// 5. Generate the tsf instance

	struct tsf_hydra hydra;
	hydra.phdrs = preset_headers;
	hydra.phdrNum = 2;
	hydra.pbags = preset_zones;
	hydra.pbagNum = 2;
	hydra.pgens = preset_generators;
	hydra.pgenNum = 2;
	hydra.pmods = preset_modulators;
	hydra.pmodNum = 1;
	hydra.insts = instrument_headers;
	hydra.instNum = 2;
	hydra.ibags = instrument_zones;
	hydra.ibagNum = (tsf_u16) instrument_zones_length;
	hydra.igens = instrument_generators;
	hydra.igenNum = (tsf_u16) instrument_generator_count;
	hydra.imods = instrument_modulators;
	hydra.imodNum = 1;
	hydra.shdrs = sample_headers;
	hydra.shdrNum = (tsf_u16) sample_headers_length;

	tsf* res = *out = Dm_alloc(sizeof(tsf));
	if (res == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	if (!tsf_load_presets(res, &hydra, (tsf_u32) sample_count)) {
		Dm_report(DmLogLevel_ERROR, "DmSynth: Failed to load tsf presets");
		return DmResult_INTERNAL_ERROR;
	}

	res->outSampleRate = 44100.0F;
	res->fontSamples = samples;

	if (!tsf_set_max_voices(res, (int) (dls->region_count * 4))) {
		Dm_report(DmLogLevel_ERROR, "DmSynth: Failed to set tsf voice count");
		return DmResult_INTERNAL_ERROR;
	}

	tsf_channel_set_bank_preset(res, 0, 0, 0);

	Dm_free(sample_headers);
	Dm_free(instrument_zones);
	Dm_free(instrument_generators);

	return DmResult_SUCCESS;
}