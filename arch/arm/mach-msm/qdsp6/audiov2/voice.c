/* Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <mach/debug_audio_mm.h>
#include <mach/msm_qdsp6_audiov2.h>
#include "../dal.h"
#include "dal_voice.h"

struct voice_struct {
	struct dal_client *cvd;
	struct apr_command_pkt apr_pkt;
	struct completion compl;
};

static struct voice_struct voice;

static int cvd_send_response(void)
{
	struct apr_command_pkt *pkt;
	uint16_t src_addr;
	uint16_t src_token;
	uint16_t dst_token;
	uint16_t dst_addr;

	pkt = &voice.apr_pkt;
	src_addr = pkt->dst_addr;
	dst_addr = pkt->src_addr;
	src_token = pkt->dst_token;
	dst_token = pkt->src_token;

	pkt->header &= ~APR_PKTV1_TYPE_MASK;
	pkt->header |= APR_SET_FIELD(APR_PKTV1_TYPE, APR_PKTV1_TYPE_EVENT_V);
	pkt->src_addr = src_addr;
	pkt->dst_addr = dst_addr;
	pkt->src_token = src_token;
	pkt->dst_token = dst_token;
	pkt->opcode = APR_IBASIC_RSP_RESULT;

	dal_call(voice.cvd, VOICE_OP_CONTROL, 5, pkt,
			sizeof(struct apr_command_pkt),
			pkt, sizeof(u32));
	return 0;
}

static int cvd_process_voice_setup(void)
{
	q6voice_setup();
	cvd_send_response();
	return 0;
}

static int cvd_process_voice_teardown(void)
{
	q6voice_teardown();
	cvd_send_response();
	return 0;
}

static int cvd_process_set_network(void)
{
	cvd_send_response();
	return 0;
}

static int voice_thread(void *data)
{
	while (!kthread_should_stop()) {
		wait_for_completion(&voice.compl);
		init_completion(&voice.compl);

		switch (voice.apr_pkt.opcode) {

		case APR_OP_CMD_CREATE:
			cvd_send_response();
			break;
		case VOICE_OP_CMD_BRINGUP:
			cvd_process_voice_setup();
			break;
		case APR_OP_CMD_DESTROY:
			cvd_send_response();
			break;
		case VOICE_OP_CMD_TEARDOWN:
			cvd_process_voice_teardown();
			break;
		case VOICE_OP_CMD_SET_NETWORK:
			cvd_process_set_network();
			break;
		default:
			MM_ERR("Undefined event\n");

		}
	}
	return 0;
}

static void remote_cb_function(void *data, int len, void *cookie)
{
	struct apr_command_hdr *apr = data + 2*sizeof(uint32_t);

	memcpy(&voice.apr_pkt, apr, sizeof(struct apr_command_pkt));

	if (len <= 0) {
		MM_ERR("unexpected event with length %d \n", len);
		return;
	}

	MM_DBG("APR = %x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n",
	apr->header,
	apr->reserved1,
	apr->src_addr,
	apr->dst_addr,
	apr->ret_addr,
	apr->src_token,
	apr->dst_token,
	apr->ret_token,
	apr->context,
	apr->opcode);

	complete(&voice.compl);
}

static int __init voice_init(void)
{
	int res = 0;
	struct task_struct *task;
	u32 tmp[2];

	tmp[0] = sizeof(u32);
	tmp[1] = 0;

	voice.cvd = dal_attach(VOICE_DAL_DEVICE, VOICE_DAL_PORT, 0,
			remote_cb_function, 0);

	if (!voice.cvd) {
		MM_ERR("audio_init: cannot attach to cvd\n");
		res = -ENODEV;
		goto done;
	}

	if (check_version(voice.cvd, VOICE_DAL_VERSION) != 0) {
		pr_err("Incompatible cvd version\n");
		res = -ENODEV;
		goto done;
	}
	dal_call(voice.cvd, VOICE_OP_INIT, 5, tmp, sizeof(tmp),
		tmp, sizeof(u32));

	init_completion(&voice.compl);
	task = kthread_run(voice_thread, &voice, "voice_thread");

	if (IS_ERR(task)) {
		MM_ERR("Cannot start the voice thread\n");
		res = PTR_ERR(task);
		task = NULL;
	} else
		goto done;

done:
	return res;
}

late_initcall(voice_init);