/*
 * linux/sound/soc/lf1000/didj-cs43l22.c
 *
 * ALSA Machine driver for the LeapFrog LF1000 Didj style game console.
 * Supports the Cirrus Logic CS43L22 and CS42L52 chips connected to the LF1000
 * SoC's I2S controller.
 *
 * Authors: Scott Esters <sesters@leapfrog.com>
 *
 * This program is free software; you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License version 2 as 
 * published by the Free Software Foundation. 
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/sysfs.h>

#include <sound/core.h>
#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/jack.h>

#include <mach/gpio.h>

#include "lf1000-pcm.h"
#include "lf1000-i2s.h"
#include "didj-cs43l22.h"
#include "../codecs/cs43l22.h"

#define DIDJ_DEFAULT_RATE	32000

static struct snd_soc_card snd_soc_didj_cs43l22;

struct lfjack_priv {
	bool			run;
	bool			last_jack;	/* last jack sample */
	bool			update_jack;	/* update jack if not forced */
	int			reg26_mixer_jack_low;
	int			reg26_mixer_jack_high;
	unsigned int		force_audio;
	unsigned int		force_mixer;
	struct task_struct	*detect_thread;
	struct semaphore	detect_thread_done;
	struct dentry		*debug;
	struct snd_soc_codec	*codec;
};

static struct lfjack_priv *lfjack = NULL;

/*
 * sysfs interface
 */

static ssize_t show_force_audio(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return(sprintf(buf,"%u\n", lfjack->force_audio));
}

static ssize_t set_force_audio(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	lfjack->force_audio = value;
	switch(lfjack->force_audio) {
	case AUDIO_NORMAL:
		lfjack->codec->write(lfjack->codec, CS43L22_SPKCTL, 
			CS43L22_SPKCTL_NORMAL);
		break;
	case AUDIO_HEADPHONES_ONLY:
		lfjack->codec->write(lfjack->codec, CS43L22_SPKCTL, 
			CS43L22_SPKCTL_HEADPHONES_ONLY);
		break;
	default:
		break;
	}
	lfjack->update_jack = 1;	/* update jack if not forced */
	return count;
}

static DEVICE_ATTR(force_audio, S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR|S_IWGRP|S_IWOTH,
	show_force_audio, set_force_audio);

static ssize_t show_force_mixer(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return(sprintf(buf,"%u\n", lfjack->force_mixer));
}

static ssize_t set_force_mixer(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int value;

	if (sscanf(buf, "%x", &value) != 1)
		return -EINVAL;

	lfjack->force_mixer = value;
	switch(lfjack->force_mixer) {
	case MIXER_NORMAL:
		break;
	case MIXER_MONO:
		lfjack->codec->write(lfjack->codec, CS43L22_MIXER,
				CS43L22_MIXER_MONO);
		break;
	default:
		break;
	}

	lfjack->update_jack = 1;	/* update jack if not forced */
	return count;
}

static DEVICE_ATTR(force_mixer, S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR|S_IWGRP|S_IWOTH,
	show_force_mixer, set_force_mixer);

static struct attribute *cs43l22_attributes[] = {
	&dev_attr_force_audio.attr,
	&dev_attr_force_mixer.attr,
	NULL
};

static struct attribute_group cs43l22_attr_group = {
	.attrs = cs43l22_attributes
};


/*
 * Board ops
 */

static int didj_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;
	int ret;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S);
	if (ret)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, 0, DIDJ_DEFAULT_RATE, 1);
	if (ret)
		return ret;

	return 0;
}

static int didj_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->dai->cpu_dai;

	snd_soc_dai_set_sysclk(cpu_dai, 0, params_rate(params), 1);
	
	return 0;
}

static struct snd_soc_ops didj_ops = {
	.startup	= didj_startup,
	.hw_params	= didj_hw_params,
};

/* didj digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link didj_dai_cs43l22[] = {
	{
		.name = "CS43L22",
		.stream_name = "CS43L22",
		.cpu_dai = &lf1000_i2s_dai,
		.codec_dai = &cs43l22_dai,
		.ops = &didj_ops,
	},
};

/* didj audio machine driver */
static struct snd_soc_card snd_soc_didj_cs43l22 = {
	.name = "Didj-CS43L22",
	.platform = &lf1000_soc_platform,
	.dai_link = didj_dai_cs43l22,
	.num_links = ARRAY_SIZE(didj_dai_cs43l22),
};

/* didj audio private data */
static struct cs43l22_setup_data didj_cs43l22_setup = {
	.i2c_bus	= 0,
	.i2c_address	= CS43L22_ADDR,
};

/* didj audio subsystem */
static struct snd_soc_device didj_snd_devdata_cs43l22 = {
	.card = &snd_soc_didj_cs43l22,
	.codec_dev = &soc_codec_dev_cs43l22,
	.codec_data = &didj_cs43l22_setup,
	.dev = NULL, /* set in didj_audio_probe */
};

static int lfjack_detect(void *data)
{
	struct lfjack_priv *priv = (struct lfjack_priv *)data;
	int cur_jack;

	while (1) {
		if (!priv->run) {
			up(&priv->detect_thread_done);
			do_exit(0);
		}

		cur_jack = gpio_get_val(lf1000_l2p_port(HEADPHONE_JACK),
			lf1000_l2p_pin(HEADPHONE_JACK));

		/* ignore if mixer forced already */
		if (priv->force_mixer == MIXER_NORMAL &&
		    priv->force_mixer == MIXER_NORMAL &&
		    (cur_jack != priv->last_jack || priv->update_jack)) {
			/* change in status? */
			if (!cur_jack) {
				priv->codec->write(priv->codec, CS43L22_MIXER,
					priv->reg26_mixer_jack_low);
			} else {
				priv->codec->write(priv->codec, CS43L22_MIXER,
					priv->reg26_mixer_jack_high);
			}
			priv->last_jack = cur_jack;
			priv->update_jack = 0;
		}
		msleep(250);	/* sample 4 times a second */
	}

}

static struct platform_device *didj_snd_device = NULL;

static int didj_audio_probe(struct platform_device *pdev)
{
	struct snd_soc_codec *codec;
	int ret;
	int i;

	dev_info(&pdev->dev, "%s\n", __FUNCTION__);

	didj_snd_device = platform_device_alloc("soc-audio", -1);
	if (!didj_snd_device)
		return -ENOMEM;

	platform_set_drvdata(didj_snd_device, &didj_snd_devdata_cs43l22);
	didj_snd_devdata_cs43l22.dev = &didj_snd_device->dev;
	
	ret = platform_device_add(didj_snd_device);
	if (ret) {
		dev_err(&pdev->dev, "can't add sound device\n");
		platform_device_put(didj_snd_device);
		return ret;
	}

	/* program codec defaults */
	codec = didj_snd_devdata_cs43l22.card->codec;
	for (i = 0; i < ARRAY_SIZE(cs43L22_settings); i++)
		codec->write(codec, cs43L22_settings[i][0],
				cs43L22_settings[i][1]);

	/* setup headphone jack monitoring */
	lfjack = kzalloc(sizeof(struct lfjack_priv), GFP_KERNEL);
	if (!lfjack)
		return -ENOMEM;

	lfjack->codec = codec;	/* point at codec */

	/* configure_pin(PORT, PIN, FUNCTION IN=0, PULLUP=1, VALUE) */
	gpio_configure_pin(lf1000_l2p_port(HEADPHONE_JACK),
		lf1000_l2p_pin(HEADPHONE_JACK), GPIO_GPIOFN, 0, 1, 0);

	/* initialize headphone state */
	lfjack->last_jack = (gpio_get_val(lf1000_l2p_port(HEADPHONE_JACK),
		lf1000_l2p_pin(HEADPHONE_JACK)) == 1);

	lfjack->update_jack = 1;	/* update headphone/mixer setting */

	/* set cs43L22 register control values */
	lfjack->reg26_mixer_jack_low = CS43L22_MIXER_MONO;
	lfjack->reg26_mixer_jack_high = CS43L22_MIXER_STEREO;

	lfjack->debug = debugfs_create_dir("lfjack", NULL);
	if (IS_ERR(lfjack->debug))
		lfjack->debug = NULL;

	if (lfjack->debug) {
		debugfs_create_bool("last_jack", S_IRUGO, lfjack->debug,
			(u32 *)&lfjack->last_jack);
	}

	/* start jack thread */
	sema_init(&lfjack->detect_thread_done, 0);
	lfjack->run = 1;
	lfjack->detect_thread = kthread_run(lfjack_detect, (void *)lfjack,
		"lfjack-detect");

	sysfs_create_group(&pdev->dev.kobj, &cs43l22_attr_group);
	return 0;
}

static int __devexit didj_audio_remove(struct platform_device *pdev)
{
	lfjack->run = 0;
	down(&lfjack->detect_thread_done);
	if (lfjack->debug)
		debugfs_remove(lfjack->debug);
	kfree(lfjack);
	sysfs_remove_group(&pdev->dev.kobj, &cs43l22_attr_group);
	platform_device_unregister(didj_snd_device);
	didj_snd_device = NULL;

	return 0;
}

static struct platform_driver didj_audio_driver = {
	.probe	= didj_audio_probe,
	.remove	= didj_audio_remove,
	.driver	= {
		.name	= "didj-asoc",
		.owner	= THIS_MODULE,
	},
};

static int __init didj_audio_init(void)
{
	return platform_driver_register(&didj_audio_driver);
}
module_init(didj_audio_init);

static void __exit didj_audio_exit(void)
{
	platform_driver_unregister(&didj_audio_driver);
}
module_exit(didj_audio_exit);

MODULE_AUTHOR("Scott Esters <sesters@leapfrog.com>");
MODULE_DESCRIPTION("ALSA SoC Didj Consoles");
MODULE_LICENSE("GPL");
