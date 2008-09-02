#include "meta.h"
#include "../layout/layout.h"
#include "../coding/coding.h"
#include "../util.h"

/* If these variables are packed properly in the struct (one after another)
 * then this is actually how they are laid out in the file, albeit big-endian */

struct dsp_header {
    uint32_t sample_count;
    uint32_t nibble_count;
    uint32_t sample_rate;
    uint16_t loop_flag;
    uint16_t format;
    uint32_t loop_start_offset;
    uint32_t loop_end_offset;
    uint32_t ca;
    int16_t coef[16]; /* really 8x2 */
    uint16_t gain;
    uint16_t initial_ps;
    int16_t initial_hist1;
    int16_t initial_hist2;
    uint16_t loop_ps;
    int16_t loop_hist1;
    int16_t loop_hist2;
};

/* nonzero on failure */
static int read_dsp_header(struct dsp_header *header, off_t offset, STREAMFILE *file) {
    int i;
    uint8_t buf[0x4a]; /* usually padded out to 0x60 */
    if (read_streamfile(buf, offset, 0x4a, file) != 0x4a) return 1;

    header->sample_count =
        get_32bitBE(buf+0x00);
    header->nibble_count =
        get_32bitBE(buf+0x04);
    header->sample_rate =
        get_32bitBE(buf+0x08);
    header->loop_flag =
        get_16bitBE(buf+0x0c);
    header->format =
        get_16bitBE(buf+0x0e);
    header->loop_start_offset =
        get_32bitBE(buf+0x10);
    header->loop_end_offset =
        get_32bitBE(buf+0x14);
    header->ca =
        get_32bitBE(buf+0x18);
    for (i=0; i < 16; i++)
        header->coef[i] =
        get_16bitBE(buf+0x1c+i*2);
    header->gain =
        get_16bitBE(buf+0x3c);
    header->initial_ps =
        get_16bitBE(buf+0x3e);
    header->initial_hist1 =
        get_16bitBE(buf+0x40);
    header->initial_hist2 =
        get_16bitBE(buf+0x42);
    header->loop_ps =
        get_16bitBE(buf+0x44);
    header->loop_hist1 =
        get_16bitBE(buf+0x46);
    header->loop_hist2 =
        get_16bitBE(buf+0x48);

    return 0;
}

/* the standard .dsp, as generated by DSPADPCM.exe */

VGMSTREAM * init_vgmstream_ngc_dsp_std(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    struct dsp_header header;
    const off_t start_offset = 0x60;
    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("dsp",filename_extension(filename))) goto fail;

    if (read_dsp_header(&header, 0, streamFile)) goto fail;

    /* check initial predictor/scale */
    if (header.initial_ps != (uint8_t)read_8bit(start_offset,streamFile))
        goto fail;

    /* check type==0 and gain==0 */
    if (header.format || header.gain)
        goto fail;

    /* Check for a matching second header. If we find one and it checks
     * out thoroughly, we're probably not dealing with a genuine mono DSP.
     * In many cases these will pass all the other checks, including the
     * predictor/scale check if the first byte is 0 */
    {
        struct dsp_header header2;

        read_dsp_header(&header2, 0x60, streamFile);

        if (header.sample_count == header2.sample_count &&
            header.nibble_count == header2.nibble_count &&
            header.sample_rate == header2.sample_rate &&
            header.loop_flag == header2.loop_flag) goto fail;
    }
        
    if (header.loop_flag) {
        off_t loop_off;
        /* check loop predictor/scale */
        loop_off = header.loop_start_offset/16*8;
        if (header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
            goto fail;
    }

    /* compare num_samples with nibble count */
    /*
    fprintf(stderr,"num samples (literal): %d\n",read_32bitBE(0,streamFile));
    fprintf(stderr,"num samples (nibbles): %d\n",dsp_nibbles_to_samples(read_32bitBE(4,streamFile)));
    */

    /* build the VGMSTREAM */


    vgmstream = allocate_vgmstream(1,header.loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = header.sample_count;
    vgmstream->sample_rate = header.sample_rate;

    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            header.loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            header.loop_end_offset)+1;

    /* don't know why, but it does happen*/
    if (vgmstream->loop_end_sample > vgmstream->num_samples)
        vgmstream->loop_end_sample = vgmstream->num_samples;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_none;
    vgmstream->meta_type = meta_DSP_STD;

    /* coeffs */
    for (i=0;i<16;i++)
        vgmstream->ch[0].adpcm_coef[i] = header.coef[i];
    
    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = header.initial_hist2;

    /* open the file for reading */
    vgmstream->ch[0].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);

    if (!vgmstream->ch[0].streamfile) goto fail;

    vgmstream->ch[0].channel_start_offset=
        vgmstream->ch[0].offset=start_offset;

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* Some very simple stereo variants of standard dsp just use the standard header
 * twice and add interleave, or just concatenate the channels. We'll support
 * them all here.
 * Note that Cstr isn't here, despite using the form of the standard header,
 * because its loop values are wacky. */

/* .stm
 * Used in Paper Mario 2, Fire Emblem: Path of Radiance, Cubivore
 * I suspected that this was an Intelligent Systems format, but its use in
 * Cubivore calls that into question. */
VGMSTREAM * init_vgmstream_ngc_dsp_stm(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    struct dsp_header ch0_header, ch1_header;
    int i;
    int stm_header_sample_rate;
    int channel_count;
    const off_t start_offset = 0x100;
    off_t first_channel_size;
    off_t second_channel_start;

    /* check extension, case insensitive */
    /* to avoid collision with Scream Tracker 2 Modules, also ending in .stm
     * and supported by default in Winamp, it was policy in the old days to
     * rename these files to .dsp */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("stm",filename_extension(filename)) &&
            strcasecmp("dsp",filename_extension(filename))) goto fail;

    /* check intro magic */
    if (read_16bitBE(0, streamFile) != 0x0200) goto fail;

    channel_count = read_32bitBE(4, streamFile);
    /* only stereo and mono are known */
    if (channel_count != 1 && channel_count != 2) goto fail;

    first_channel_size = read_32bitBE(8, streamFile);
    /* this is bad rounding, wastes space, but it looks like that's what's
     * used */
    second_channel_start = ((start_offset+first_channel_size)+0x20)/0x20*0x20;

    /* an additional check */
    stm_header_sample_rate = (uint16_t)read_16bitBE(2, streamFile);

    /* read the DSP headers */
    if (read_dsp_header(&ch0_header, 0x40, streamFile)) goto fail;
    if (channel_count == 2) {
        if (read_dsp_header(&ch1_header, 0xa0, streamFile)) goto fail;
    }

    /* checks for fist channel */
    {
        if (ch0_header.sample_rate != stm_header_sample_rate) goto fail;

        /* check initial predictor/scale */
        if (ch0_header.initial_ps != (uint8_t)read_8bit(start_offset, streamFile))
            goto fail;

        /* check type==0 and gain==0 */
        if (ch0_header.format || ch0_header.gain)
            goto fail;

        if (ch0_header.loop_flag) {
            off_t loop_off;
            /* check loop predictor/scale */
            loop_off = ch0_header.loop_start_offset/16*8;
            if (ch0_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
                goto fail;
        }
    }


    /* checks for second channel */
    if (channel_count == 2) {
        if (ch1_header.sample_rate != stm_header_sample_rate) goto fail;

        /* check for agreement with first channel header */
        if (
            ch0_header.sample_count != ch1_header.sample_count ||
            ch0_header.nibble_count != ch1_header.nibble_count ||
            ch0_header.loop_flag != ch1_header.loop_flag ||
            ch0_header.loop_start_offset != ch1_header.loop_start_offset ||
            ch0_header.loop_end_offset != ch1_header.loop_end_offset
           ) goto fail;

        /* check initial predictor/scale */
        if (ch1_header.initial_ps != (uint8_t)read_8bit(second_channel_start, streamFile))
            goto fail;

        /* check type==0 and gain==0 */
        if (ch1_header.format || ch1_header.gain)
            goto fail;

        if (ch1_header.loop_flag) {
            off_t loop_off;
            /* check loop predictor/scale */
            loop_off = ch1_header.loop_start_offset/16*8;
            /*printf("loop_start_offset=%x\nloop_ps=%x\nloop_off=%x\n",ch1_header.loop_start_offset,ch1_header.loop_ps,second_channel_start+loop_off);*/
            if (ch1_header.loop_ps != (uint8_t)read_8bit(second_channel_start+loop_off,streamFile))
                goto fail;
        }
    }

    /* build the VGMSTREAM */

    vgmstream = allocate_vgmstream(channel_count, ch0_header.loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = ch0_header.sample_count;
    vgmstream->sample_rate = ch0_header.sample_rate;

    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            ch0_header.loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            ch0_header.loop_end_offset)+1;

    /* don't know why, but it does happen*/
    if (vgmstream->loop_end_sample > vgmstream->num_samples)
        vgmstream->loop_end_sample = vgmstream->num_samples;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_none;
    vgmstream->meta_type = meta_DSP_STM;

    /* coeffs */
    for (i=0;i<16;i++)
        vgmstream->ch[0].adpcm_coef[i] = ch0_header.coef[i];

    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = ch0_header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = ch0_header.initial_hist2;

    if (channel_count == 2) {
        /* coeffs */
        for (i=0;i<16;i++)
            vgmstream->ch[1].adpcm_coef[i] = ch1_header.coef[i];

        /* initial history */
        /* always 0 that I've ever seen, but for completeness... */
        vgmstream->ch[1].adpcm_history1_16 = ch1_header.initial_hist1;
        vgmstream->ch[1].adpcm_history2_16 = ch1_header.initial_hist2;
    }

    /* open the file for reading */
    vgmstream->ch[0].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);

    if (!vgmstream->ch[0].streamfile) goto fail;

    vgmstream->ch[0].channel_start_offset=
        vgmstream->ch[0].offset=start_offset;

    if (channel_count == 2) {
        vgmstream->ch[1].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);

        if (!vgmstream->ch[1].streamfile) goto fail;

        vgmstream->ch[1].channel_start_offset=
            vgmstream->ch[1].offset=second_channel_start;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* mpdsp: looks like a standard .dsp header, but the data is actually
 * interleaved stereo 
 * The files originally had a .dsp extension, we rename them to .mpdsp so we
 * can catch this.
 */

VGMSTREAM * init_vgmstream_ngc_mpdsp(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    struct dsp_header header;
    const off_t start_offset = 0x60;
    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("mpdsp",filename_extension(filename))) goto fail;

    if (read_dsp_header(&header, 0, streamFile)) goto fail;

    /* none have loop flag set, save us from loop code that involves them */
    if (header.loop_flag) goto fail;

    /* check initial predictor/scale */
    if (header.initial_ps != (uint8_t)read_8bit(start_offset,streamFile))
        goto fail;

    /* check type==0 and gain==0 */
    if (header.format || header.gain)
        goto fail;
        
    /* build the VGMSTREAM */


    /* no loop flag, but they do loop */
    vgmstream = allocate_vgmstream(2,0);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = header.sample_count/2;
    vgmstream->sample_rate = header.sample_rate;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = 0xf000;
    vgmstream->meta_type = meta_DSP_MPDSP;

    /* coeffs */
    for (i=0;i<16;i++) {
        vgmstream->ch[0].adpcm_coef[i] = header.coef[i];
        vgmstream->ch[1].adpcm_coef[i] = header.coef[i];
    }
    
    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = header.initial_hist2;
    vgmstream->ch[1].adpcm_history1_16 = header.initial_hist1;
    vgmstream->ch[1].adpcm_history2_16 = header.initial_hist2;

    /* open the file for reading */
    for (i=0;i<2;i++) {
        vgmstream->ch[i].streamfile = streamFile->open(streamFile,filename,
                vgmstream->interleave_block_size);

        if (!vgmstream->ch[i].streamfile) goto fail;

        vgmstream->ch[i].channel_start_offset=
            vgmstream->ch[i].offset=start_offset+
            vgmstream->interleave_block_size*i;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* str: a very simple header format with implicit loop values
 * it's allways in interleaved stereo format
 */
VGMSTREAM * init_vgmstream_ngc_str(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    const off_t start_offset = 0x60;
    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("str",filename_extension(filename))) goto fail;

	/* always 0xFAAF0001 @ offset 0 */
    if (read_32bitBE(0x00,streamFile)!=0xFAAF0001) goto fail;

    /* build the VGMSTREAM */
    /* always loop & stereo */
    vgmstream = allocate_vgmstream(2,1);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = read_32bitBE(0x08,streamFile);
    vgmstream->sample_rate = read_32bitBE(0x04,streamFile);

	/* always loop to the beginning */
	vgmstream->loop_start_sample=0;
	vgmstream->loop_end_sample=vgmstream->num_samples;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = read_32bitBE(0x0C,streamFile);
    vgmstream->meta_type = meta_DSP_STR;

    /* coeffs */
    for (i=0;i<16;i++) {
        vgmstream->ch[0].adpcm_coef[i] = read_16bitBE(0x10+(i*2),streamFile);
        vgmstream->ch[1].adpcm_coef[i] = read_16bitBE(0x30+(i*2),streamFile);
    }
    
    /* open the file for reading */
    for (i=0;i<2;i++) {
        vgmstream->ch[i].streamfile = streamFile->open(streamFile,filename,
                vgmstream->interleave_block_size);

        if (!vgmstream->ch[i].streamfile) goto fail;

        vgmstream->ch[i].channel_start_offset=
            vgmstream->ch[i].offset=start_offset+
            vgmstream->interleave_block_size*i;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* a bunch of formats that are identical except for file extension,
 * but have different interleaves */

VGMSTREAM * init_vgmstream_ngc_dsp_std_int(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    const off_t start_offset = 0xc0;
    off_t interleave;
    int meta_type;

    struct dsp_header ch0_header,ch1_header;

    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strlen(filename) > 7 && !strcasecmp("_lr.dsp",filename+strlen(filename)-7)) {
        /* Bomberman Jetters */
        interleave = 0x14180;
        meta_type = meta_DSP_JETTERS;
    } else if (!strcasecmp("mss",filename_extension(filename))) {
        interleave = 0x1000;
        meta_type = meta_DSP_MSS;
    } else if (!strcasecmp("gcm",filename_extension(filename))) {
        interleave = 0x8000;
        meta_type = meta_DSP_GCM;
    } else goto fail;

    if (read_dsp_header(&ch0_header, 0, streamFile)) goto fail;
    if (read_dsp_header(&ch1_header, 0x60, streamFile)) goto fail;

    /* check initial predictor/scale */
    if (ch0_header.initial_ps != (uint8_t)read_8bit(start_offset,streamFile))
        goto fail;
    if (ch1_header.initial_ps != (uint8_t)read_8bit(start_offset+interleave,streamFile))
        goto fail;

    /* check type==0 and gain==0 */
    if (ch0_header.format || ch0_header.gain ||
        ch1_header.format || ch1_header.gain)
        goto fail;

    /* check for agreement */
    if (
            ch0_header.sample_count != ch1_header.sample_count ||
            ch0_header.nibble_count != ch1_header.nibble_count ||
            ch0_header.sample_rate != ch1_header.sample_rate ||
            ch0_header.loop_flag != ch1_header.loop_flag ||
            ch0_header.loop_start_offset != ch1_header.loop_start_offset ||
            ch0_header.loop_end_offset != ch1_header.loop_end_offset
       ) goto fail;

    if (ch0_header.loop_flag) {
        off_t loop_off;
        /* check loop predictor/scale */
        loop_off = ch0_header.loop_start_offset/16*8;
        loop_off = (loop_off/interleave*interleave*2) + (loop_off%interleave);
        if (ch0_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
            goto fail;
        if (ch1_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off+interleave,streamFile))
            goto fail;
    }

    /* build the VGMSTREAM */

    vgmstream = allocate_vgmstream(2,ch0_header.loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = ch0_header.sample_count;
    vgmstream->sample_rate = ch0_header.sample_rate;

    /* TODO: adjust for interleave? */
    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            ch0_header.loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            ch0_header.loop_end_offset)+1;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = interleave;
    vgmstream->meta_type = meta_type;

    /* coeffs */
    for (i=0;i<16;i++) {
        vgmstream->ch[0].adpcm_coef[i] = ch0_header.coef[i];
        vgmstream->ch[1].adpcm_coef[i] = ch1_header.coef[i];
    }
    
    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = ch0_header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = ch0_header.initial_hist2;
    vgmstream->ch[1].adpcm_history1_16 = ch1_header.initial_hist1;
    vgmstream->ch[1].adpcm_history2_16 = ch1_header.initial_hist2;

    /* open the file for reading */
    for (i=0;i<2;i++) {
        vgmstream->ch[i].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);

        if (!vgmstream->ch[i].streamfile) goto fail;

        vgmstream->ch[i].channel_start_offset=
            vgmstream->ch[i].offset=start_offset+i*interleave;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* sadb - .SAD files, two standard DSP headers */
VGMSTREAM * init_vgmstream_sadb(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    off_t start_offset;
    off_t interleave;

    struct dsp_header ch0_header,ch1_header;
    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("sad",filename_extension(filename))) goto fail;

    if (read_dsp_header(&ch0_header, 0x80, streamFile)) goto fail;
    if (read_dsp_header(&ch1_header, 0xe0, streamFile)) goto fail;

    /* check header magic */
    if (read_32bitBE(0x0,streamFile) != 0x73616462) goto fail; /* "sadb" */

    start_offset = read_32bitBE(0x48,streamFile);
    interleave = 16;

    /* check initial predictor/scale */
    if (ch0_header.initial_ps != (uint8_t)read_8bit(start_offset,streamFile))
        goto fail;
    if (ch1_header.initial_ps != (uint8_t)read_8bit(start_offset+interleave,streamFile))
        goto fail;

    /* check type==0 and gain==0 */
    if (ch0_header.format || ch0_header.gain ||
        ch1_header.format || ch1_header.gain)
        goto fail;

    /* check for agreement */
    if (
            ch0_header.sample_count != ch1_header.sample_count ||
            ch0_header.nibble_count != ch1_header.nibble_count ||
            ch0_header.sample_rate != ch1_header.sample_rate ||
            ch0_header.loop_flag != ch1_header.loop_flag ||
            ch0_header.loop_start_offset != ch1_header.loop_start_offset ||
            ch0_header.loop_end_offset != ch1_header.loop_end_offset
       ) goto fail;

    if (ch0_header.loop_flag) {
        off_t loop_off;
        /* check loop predictor/scale */
        loop_off = ch0_header.loop_start_offset/16*8;
        loop_off = (loop_off/interleave*interleave*2) + (loop_off%interleave);
        if (ch0_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
            goto fail;
        if (ch1_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off+interleave,streamFile))
            goto fail;
    }

    /* build the VGMSTREAM */

    vgmstream = allocate_vgmstream(2,ch0_header.loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = ch0_header.sample_count;
    vgmstream->sample_rate = ch0_header.sample_rate;

    /* TODO: adjust for interleave? */
    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            ch0_header.loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            ch0_header.loop_end_offset)+1;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = interleave;
    vgmstream->meta_type = meta_DSP_SADB;

    /* coeffs */
    for (i=0;i<16;i++) {
        vgmstream->ch[0].adpcm_coef[i] = ch0_header.coef[i];
        vgmstream->ch[1].adpcm_coef[i] = ch1_header.coef[i];
    }
    
    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = ch0_header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = ch0_header.initial_hist2;
    vgmstream->ch[1].adpcm_history1_16 = ch1_header.initial_hist1;
    vgmstream->ch[1].adpcm_history2_16 = ch1_header.initial_hist2;

    vgmstream->ch[0].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);
    vgmstream->ch[1].streamfile = vgmstream->ch[0].streamfile;

    if (!vgmstream->ch[0].streamfile) goto fail;
    /* open the file for reading */
    for (i=0;i<2;i++) {
        vgmstream->ch[i].channel_start_offset=
            vgmstream->ch[i].offset=start_offset+i*interleave;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* AMTS - .amts files */
VGMSTREAM * init_vgmstream_amts(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    off_t start_offset;
    off_t interleave;
	int channel_count;

    struct dsp_header ch0_header,ch1_header;
    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("amts",filename_extension(filename))) goto fail;

    /* check header magic */
    if (read_32bitBE(0x0,streamFile) != 0x414D5453) goto fail; /* "sadb" */
	
	channel_count=read_32bitBE(0x14,streamFile);
    start_offset = 0x800;
    interleave = read_32bitBE(0x08,streamFile);

	if (read_dsp_header(&ch0_header, 0x20, streamFile)) goto fail;

    /* check initial predictor/scale */
    if (ch0_header.initial_ps != (uint8_t)read_8bit(start_offset,streamFile))
        goto fail;

	if(channel_count==2) {
		if (read_dsp_header(&ch1_header, 0x80, streamFile)) goto fail;

	    if (ch1_header.initial_ps != (uint8_t)read_8bit(start_offset+interleave,streamFile))
		    goto fail;

		/* check type==0 and gain==0 */
		if (ch0_header.format || ch0_header.gain ||
			ch1_header.format || ch1_header.gain)
			goto fail;

		/* check for agreement */
		if (
				ch0_header.sample_count != ch1_header.sample_count ||
				ch0_header.nibble_count != ch1_header.nibble_count ||
				ch0_header.sample_rate != ch1_header.sample_rate ||
				ch0_header.loop_flag != ch1_header.loop_flag ||
				ch0_header.loop_start_offset != ch1_header.loop_start_offset ||
				ch0_header.loop_end_offset != ch1_header.loop_end_offset
		   ) goto fail;

		if (ch0_header.loop_flag) {
			off_t loop_off;
			/* check loop predictor/scale */
			loop_off = ch0_header.loop_start_offset/16*8;
			loop_off = (loop_off/interleave*interleave*2) + (loop_off%interleave);
			if (ch0_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
				goto fail;
			if (ch1_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off+interleave,streamFile))
				goto fail;
		}

	}

    /* build the VGMSTREAM */

    vgmstream = allocate_vgmstream(channel_count,ch0_header.loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = ch0_header.sample_count;
    vgmstream->sample_rate = ch0_header.sample_rate;

    /* TODO: adjust for interleave? */
    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            ch0_header.loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            ch0_header.loop_end_offset)+1;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = interleave;
    vgmstream->meta_type = meta_DSP_AMTS;

    /* coeffs */
    for (i=0;i<16;i++) {
        vgmstream->ch[0].adpcm_coef[i] = ch0_header.coef[i];
        vgmstream->ch[1].adpcm_coef[i] = ch1_header.coef[i];
    }
    
    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = ch0_header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = ch0_header.initial_hist2;
    vgmstream->ch[0].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);

	if(channel_count==2) {
		vgmstream->ch[1].adpcm_history1_16 = ch1_header.initial_hist1;
		vgmstream->ch[1].adpcm_history2_16 = ch1_header.initial_hist2;
		vgmstream->ch[1].streamfile = vgmstream->ch[0].streamfile;
	}

    if (!vgmstream->ch[0].streamfile) goto fail;
    /* open the file for reading */
    for (i=0;i<channel_count;i++) {
        vgmstream->ch[i].channel_start_offset=
            vgmstream->ch[i].offset=start_offset+i*interleave;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

/* .wsi as found in Alone in the Dark for Wii */
/* These appear to be standard .dsp, but interleaved in a blocked format */

VGMSTREAM * init_vgmstream_wsi(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];
    struct dsp_header header[2];
    off_t start_offset[2];

    int channel_count;
    size_t est_block_size = 0;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("wsi",filename_extension(filename))) goto fail;

    /* I don't know if this is actually the channel count, or a block type
       for the first block. Won't know until I see a mono .wsi */
    channel_count = read_32bitBE(0x04,streamFile);

    /* I've only allocated two headers, and I want to be alerted if a mono
      .wsi shows up */
    if (channel_count != 2) goto fail;

    /* check for consistent block headers */
    {
        off_t check_offset;
        off_t block_size_has_been;
        int i;
       
        check_offset = read_32bitBE(0x0,streamFile);
        if (check_offset < 8) goto fail;

        block_size_has_been = check_offset;

        /* check 4 blocks, to get an idea */
        for (i=0;i<4*channel_count;i++) {
            off_t block_size;
            block_size = read_32bitBE(check_offset,streamFile);

            /* expect at least the block header */
            if (block_size < 0x10) goto fail;

            /* expect the channel numbers to alternate */
            if (i%channel_count+1 != read_32bitBE(check_offset+8,streamFile)) goto fail;

            /* expect every block in a set of channels to have the same size */
            if (i%channel_count==0) block_size_has_been = block_size;
            else if (block_size != block_size_has_been) goto fail;

            /* get an estimate of block size for buffer sizing */
            if (block_size > est_block_size) est_block_size = block_size;

            check_offset += block_size;
        }
    }

    /* look at DSP headers */

    {
        off_t check_offset;
        int i;

        check_offset = read_32bitBE(0x0,streamFile);

        for (i=0;i<channel_count;i++) {
            off_t block_size;

            block_size = read_32bitBE(check_offset,streamFile);

            /* make sure block is actually big enough to hold the dsp header
               and beginning of first frame */
            if (block_size < 0x61+0x10) goto fail;
            if (read_dsp_header(&header[i], check_offset+0x10, streamFile)) goto fail;

            start_offset[i] = check_offset + 0x60+0x10;

            /* check initial predictor/scale */
            if (header[i].initial_ps != (uint8_t)read_8bit(check_offset+0x60+0x10,streamFile))
                goto fail;

            /* check type==0 and gain==0 */
            if (header[i].format || header[i].gain)
                goto fail;

#if 0
            /* difficult to use this with blocks, but might be worth doing */
            if (header[i].loop_flag) {
                off_t loop_off;
                /* check loop predictor/scale */
                loop_off = header[i].loop_start_offset/16*8;
                if (header[i].loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
                    goto fail;
            }
#endif

            check_offset += block_size;
        }
    } /* done looking at headers */

    /* check for agreement (two channels only) */
    if (
            header[0].sample_count != header[1].sample_count ||
            header[0].nibble_count != header[1].nibble_count ||
            header[0].sample_rate != header[1].sample_rate ||
            header[0].loop_flag != header[1].loop_flag ||
            header[0].loop_start_offset != header[1].loop_start_offset ||
            header[0].loop_end_offset != header[1].loop_end_offset
       ) goto fail;


    vgmstream = allocate_vgmstream(channel_count,header[0].loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = header[0].sample_count;
    vgmstream->sample_rate = header[0].sample_rate;

    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            header[0].loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            header[0].loop_end_offset)+1;

    /* don't know why, but it does happen*/
    if (vgmstream->loop_end_sample > vgmstream->num_samples)
        vgmstream->loop_end_sample = vgmstream->num_samples;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_wsi_blocked;
    vgmstream->meta_type = meta_DSP_WSI;

    /* coeffs */
    {
        int i,j;
        for (j=0;j<channel_count;j++) {
            for (i=0;i<16;i++) {
                vgmstream->ch[j].adpcm_coef[i] = header[j].coef[i];
            }

            /* initial history */
            /* always 0 that I've ever seen, but for completeness... */
            vgmstream->ch[j].adpcm_history1_16 = header[j].initial_hist1;
            vgmstream->ch[j].adpcm_history2_16 = header[j].initial_hist2;
        }
    }


    /* open the file for reading */
    vgmstream->ch[0].streamfile = streamFile->open(streamFile,filename,est_block_size*4);

    if (!vgmstream->ch[0].streamfile) goto fail;

    wsi_block_update(read_32bitBE(0,streamFile),vgmstream);

    {
        int i;

        for (i=0;i<channel_count;i++) {
            vgmstream->ch[i].streamfile = vgmstream->ch[0].streamfile;
            vgmstream->ch[i].channel_start_offset=
                vgmstream->ch[i].offset=start_offset[i];
        }

    }

    /* first block isn't full of musics */
    vgmstream->current_block_size -= 0x60;

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}

#include "meta.h"
#include "../util.h"


/* SWD (found in Conflict - Desert Storm 1 & 2 */
VGMSTREAM * init_vgmstream_ngc_swd(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[260];

    off_t start_offset;
    off_t interleave;

    struct dsp_header ch0_header, ch1_header;
    int i;

    /* check extension, case insensitive */
    streamFile->get_name(streamFile,filename,sizeof(filename));
    if (strcasecmp("swd",filename_extension(filename))) goto fail;

    if (read_dsp_header(&ch0_header, 0x08, streamFile)) goto fail;
    if (read_dsp_header(&ch1_header, 0x68, streamFile)) goto fail;

    /* check header magic */
    if (read_16bitBE(0x00,streamFile) != 0x5053 && /* PS */
            (read_8bit(0x02,streamFile) != 0x46)) /* F */
        goto fail;

    start_offset = 0xC8;
    interleave = 0x8;

    /* check initial predictor/scale */
    if (ch0_header.initial_ps != (uint8_t)read_8bit(start_offset,streamFile))
        goto fail;
    if (ch1_header.initial_ps != (uint8_t)read_8bit(start_offset+interleave,streamFile))
        goto fail;

    /* check type==0 and gain==0 */
    if (ch0_header.format || ch0_header.gain ||
        ch1_header.format || ch1_header.gain)
        goto fail;

    /* check for agreement */
    if (
            ch0_header.sample_count != ch1_header.sample_count ||
            ch0_header.nibble_count != ch1_header.nibble_count ||
            ch0_header.sample_rate != ch1_header.sample_rate ||
            ch0_header.loop_flag != ch1_header.loop_flag ||
            ch0_header.loop_start_offset != ch1_header.loop_start_offset ||
            ch0_header.loop_end_offset != ch1_header.loop_end_offset
       ) goto fail;

    if (ch0_header.loop_flag) {
        off_t loop_off;
        /* check loop predictor/scale */
        loop_off = ch0_header.loop_start_offset/16*8;
        loop_off = (loop_off/interleave*interleave*2) + (loop_off%interleave);
        if (ch0_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off,streamFile))
            goto fail;
        if (ch1_header.loop_ps != (uint8_t)read_8bit(start_offset+loop_off+interleave,streamFile))
            goto fail;
    }

    /* build the VGMSTREAM */

    vgmstream = allocate_vgmstream(2,ch0_header.loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->num_samples = ch0_header.sample_count;
    vgmstream->sample_rate = ch0_header.sample_rate;

    /* TODO: adjust for interleave? */
    vgmstream->loop_start_sample = dsp_nibbles_to_samples(
            ch0_header.loop_start_offset);
    vgmstream->loop_end_sample =  dsp_nibbles_to_samples(
            ch0_header.loop_end_offset)+1;

    vgmstream->coding_type = coding_NGC_DSP;
    vgmstream->layout_type = layout_interleave;
    vgmstream->interleave_block_size = interleave;
    vgmstream->meta_type = meta_NGC_SWD;

    /* coeffs */
    for (i=0;i<16;i++) {
        vgmstream->ch[0].adpcm_coef[i] = ch0_header.coef[i];
        vgmstream->ch[1].adpcm_coef[i] = ch1_header.coef[i];
    }
    
    /* initial history */
    /* always 0 that I've ever seen, but for completeness... */
    vgmstream->ch[0].adpcm_history1_16 = ch0_header.initial_hist1;
    vgmstream->ch[0].adpcm_history2_16 = ch0_header.initial_hist2;
    vgmstream->ch[1].adpcm_history1_16 = ch1_header.initial_hist1;
    vgmstream->ch[1].adpcm_history2_16 = ch1_header.initial_hist2;

    vgmstream->ch[0].streamfile = streamFile->open(streamFile,filename,STREAMFILE_DEFAULT_BUFFER_SIZE);
    vgmstream->ch[1].streamfile = vgmstream->ch[0].streamfile;

    if (!vgmstream->ch[0].streamfile) goto fail;
    /* open the file for reading */
    for (i=0;i<2;i++) {
        vgmstream->ch[i].channel_start_offset=
            vgmstream->ch[i].offset=start_offset+i*interleave;
    }

    return vgmstream;

fail:
    /* clean up anything we may have opened */
    if (vgmstream) close_vgmstream(vgmstream);
    return NULL;
}