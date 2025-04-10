#include <pthread.h>
#include <signal.h>
#include "pp_common.h"
#include "pp_dv.h"

#define SERVER_IP "192.168.167.168"

static char ibv_devname[100] = "ibp59s0f0";
static char ibv_devname_vf[2][100];
static int client_sgid_idx = 3;

//#define PP_DV_OPCODE_CLIENT IBV_WR_RDMA_WRITE_WITH_IMM /* IBV_WR_SEND_WITH_IMM */

#define PP_SEND_WRID_CLIENT  0x1000
#define PP_RECV_WRID_CLIENT  0x4000

static struct pp_dv_ctx ppdv;
static struct pp_exchange_info server = {};

static int client_traffic_dv(struct pp_dv_ctx *ppdv)
{
	int num_post = PP_MAX_WR, num_comp, i, ret;
	//int opcode = MLX5_OPCODE_RDMA_WRITE_IMM;
	int opcode = MLX5_OPCODE_SEND_IMM;

	DBG("Pause 1sec before post send, opcode %d num_post %d length 0x%lx..\n",
	    opcode, num_post, ppdv->ppc.mrbuflen);
	sleep(1);

	for (i = 0; i < num_post; i++) {
		mem_string(ppdv->ppc.alias_mkey_mrbuf[0][i], ppdv->ppc.mrbuflen);
		mem_string(ppdv->ppc.alias_mkey_mrbuf[1][i], ppdv->ppc.mrbuflen);
		memcpy(ppdv->ppc.alias_mkey_mrbuf[0][i], "vf0__", 6); 
		memcpy(ppdv->ppc.alias_mkey_mrbuf[1][i] ,"vf1__", 6);
	}

	for (int i = 0; i < 100; i++) {
		ppdv->ppc.mem_region_type = MEM_REGION_TYPE_ALIAS_VF0;
		ret = pp_dv_post_send(&ppdv->ppc, &ppdv->qp, &server, num_post,
				opcode, IBV_SEND_SIGNALED);
		if (ret) {
			ERR("pp_dv_post_send failed\n");
			return ret;
		}

		ppdv->ppc.mem_region_type = MEM_REGION_TYPE_ALIAS_VF1;
		ret = pp_dv_post_send(&ppdv->ppc, &ppdv->qp, &server, num_post,
				opcode, IBV_SEND_SIGNALED);
		if (ret) {
			ERR("pp_dv_post_send failed\n");
			return ret;
		}

		num_comp = 0;
		while (num_comp < num_post * 2) {
			ret = pp_dv_poll_cq(&ppdv->cq, 1);
			usleep(1000 * 10);
			if (ret == CQ_POLL_ERR) {
				ERR("poll_cq(send) failed %d, %d/%d\n", ret, num_comp, num_post);
				return ret;
			}
			if (ret > 0)
				num_comp++;
		}
		if (i < 100)
			usleep(1000 * 100);
	}

	/* Reset the buffer so that we can check it the received data is expected */
	for (i = 0; i < num_post; i++) {
		memset(ppdv->ppc.alias_mkey_mrbuf[0][i], 0, ppdv->ppc.mrbuflen);
		memset(ppdv->ppc.alias_mkey_mrbuf[1][i], 0, ppdv->ppc.mrbuflen);
	}

	INFO("Send done (num_post %d), now recving reply...\n", num_post);
	ppdv->ppc.mem_region_type = MEM_REGION_TYPE_ALIAS_VF0;
	ret = pp_dv_post_recv(&ppdv->ppc, &ppdv->qp, num_post);
	if (ret) {
		ERR("pp_dv_post_recv failed\n");
		return ret;
	}

	num_comp = 0;
	while (num_comp < num_post) {
		ret = pp_dv_poll_cq(&ppdv->cq, 1);
		if (ret == CQ_POLL_ERR) {
			ERR("poll_cq(recv) failed %d, %d/%d\n", ret, num_comp, num_post);
			return ret;
		}
		if (ret > 0) {
			dump_msg_short(num_comp, &ppdv->ppc);
			num_comp++;
		}
	}

	INFO("Client(dv) traffic test done\n");
	return 0;
}

void *polling_mkey_modify_cq(void *arg)
{
	INFO("Start polling_mkey_modify_cq\n");
	(void)arg;
	while (true) {
		pthread_testcancel();
		int ret = pp_dv_poll_cq(&(ppdv.mkey_modify_cq), 1);
		if (ret == CQ_POLL_ERR) {
			ERR("polling_mkey_modify_cq failed %d\n", ret);
			return NULL;
		}

		if (ret > 0) {
			// Find out whose(vf0 or vf1) mkey was failed.
			usleep(1000 * 100);
			int vf_idx = 0;
			if (ppdv.mkey_modify_cq.last_access_fail_mkey == ppdv.ppc.alias_mkey[0][0])
				vf_idx = 0;
			else if (ppdv.mkey_modify_cq.last_access_fail_mkey == ppdv.ppc.alias_mkey[1][0])
				vf_idx = 1;
			INFO("Got Mkey Modify CQE for vf%d\n", vf_idx);

			if (true) {
				INFO("Start to vf context and mkey of vf%d\n", vf_idx);
				pp_ctx_cleanup(&ppdv.ppc_vf[vf_idx]);
				usleep(1000 * 1000 * 2);
				pp_ctx_init(&ppdv.ppc_vf[vf_idx], ibv_devname_vf[vf_idx], 0, NULL);
				pp_allow_other_vhca_access(&ppdv.ppc_vf[vf_idx]);

				// Init the mkey buf before assign it to alias mkey
				for (int i = 0; i < PP_MAX_WR; i++) {
					mem_string(ppdv.ppc_vf[vf_idx].mkey_mrbuf[i], ppdv.ppc.mrbuflen);
					memcpy(ppdv.ppc_vf[vf_idx].mkey_mrbuf[i], vf_idx == 0 ? "vf0__" : "vf1__", 6); 
				}
			} else {
				// Following is for "destroy mkey case", we only need to recreate mkey
				INFO("Start to recreate mkey of vf%d\n", vf_idx);
				pp_init_mkey(&ppdv.ppc_vf[vf_idx]);
			}

			INFO("Start to recreate alias mkey point to mkey of vf%d\n", vf_idx);
			pp_init_alias_mkey(&ppdv.ppc, &ppdv.ppc_vf[vf_idx], ppdv.mkey_modify_cq.cqn, vf_idx);
		}
		usleep(1000 * 10);
	}
	return NULL;
}

void sig_handler(int sig) {
    switch(sig)
    {
        case SIGUSR1:
	    pp_destroy_mkey(&(ppdv.ppc_vf[0]));
            break;
        case SIGUSR2:
	    pp_destroy_mkey(&(ppdv.ppc_vf[1]));
            break;
        default :
            break;
    }
}

int main(int argc, char *argv[])
{
    	signal(SIGUSR1, sig_handler);
    	signal(SIGUSR2, sig_handler);

	int ret;
	if (argv[1]) {
		memset(ibv_devname, 0, sizeof(ibv_devname));
		strcpy(ibv_devname, argv[1]);
	}

	if (argv[2]) {
		memset(ibv_devname_vf[0], 0, sizeof(ibv_devname_vf[0]));
		strcpy(ibv_devname_vf[0], argv[2]);
	}

	if (argv[3]) {
		memset(ibv_devname_vf[1], 0, sizeof(ibv_devname_vf[1]));
		strcpy(ibv_devname_vf[1], argv[3]);
	}

	ppdv.ppc.mem_region_type = MEM_REGION_TYPE_NONE;
	ret = pp_ctx_init(&ppdv.ppc, ibv_devname, 0, NULL);
	if (ret)
		goto out_init_ctx;

	ppdv.ppc_vf[0].mem_region_type = MEM_REGION_TYPE_DEVX;
	ret = pp_ctx_init(&ppdv.ppc_vf[0], ibv_devname_vf[0], 0, NULL);
	if (ret)
		goto out_init_ctx_vf0;

	ppdv.ppc_vf[1].mem_region_type = MEM_REGION_TYPE_DEVX;
	ret = pp_ctx_init(&ppdv.ppc_vf[1], ibv_devname_vf[1], 0, NULL);
	if (ret)
		goto out_init_ctx_vf1;

	ret = pp_allow_other_vhca_access(&ppdv.ppc_vf[0]);
	if (ret)
		goto out_allow_other_vhca_access;

	ret = pp_allow_other_vhca_access(&ppdv.ppc_vf[1]);
	if (ret)
		goto out_allow_other_vhca_access;

	ret = pp_create_cq_dv(&ppdv.ppc, &ppdv.mkey_modify_cq);
	if (ret)
		goto out_allow_other_vhca_access;

    	pthread_t thread;
    	ret = pthread_create(&thread, NULL, polling_mkey_modify_cq, 0);
	if (ret)
		goto out_create_pthread;
    	pthread_detach(thread);

	ret = pp_init_alias_mkey(&ppdv.ppc, &ppdv.ppc_vf[0], ppdv.mkey_modify_cq.cqn, 0);
	if (ret)
		goto out_init_alias_mkey;

	ret = pp_init_alias_mkey(&ppdv.ppc, &ppdv.ppc_vf[1], ppdv.mkey_modify_cq.cqn, 1);
	if (ret)
		goto out_init_alias_mkey;

	ret = pp_create_cq_dv(&ppdv.ppc, &ppdv.cq);
	if (ret)
		goto out_init_alias_mkey;

	ret = pp_create_qp_dv(&ppdv.ppc, &ppdv.cq, &ppdv.qp);
	if (ret)
		goto out_create_qp;

	ppdv.ppc.mem_region_type = MEM_REGION_TYPE_ALIAS_VF0;
	ret = pp_exchange_info(&ppdv.ppc, client_sgid_idx, ppdv.qp.qpn,
			       CLIENT_PSN, &server, SERVER_IP);
	if (ret)
		goto out_exchange;

	ret = pp_move2rts_dv(&ppdv.ppc, &ppdv.qp, client_sgid_idx,
			     CLIENT_PSN, &server);
	if (ret)
		goto out_exchange;

	ret = client_traffic_dv(&ppdv);

out_exchange:
	pp_destroy_qp_dv(&ppdv.qp);
out_create_qp:
	pp_destroy_cq_dv(&ppdv.cq);
out_init_alias_mkey:
	pthread_cancel(thread);
	usleep(1000 * 1000);
out_create_pthread:
	pp_destroy_cq_dv(&ppdv.mkey_modify_cq);
out_allow_other_vhca_access:
	pp_ctx_cleanup(&ppdv.ppc_vf[1]);
out_init_ctx_vf1:
	pp_ctx_cleanup(&ppdv.ppc_vf[0]);
out_init_ctx_vf0:
	pp_ctx_cleanup(&ppdv.ppc);
out_init_ctx:
	return ret;
}
