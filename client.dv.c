#include <pthread.h>
#include <signal.h>
#include "pp_common.h"
#include "pp_dv.h"

#define SERVER_IP "192.168.167.168"

static char ibv_devname[100] = "ibp59s0f0";
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
		mem_string(ppdv->ppc.mrbuf[i], ppdv->ppc.mrbuflen);
		*ppdv->ppc.mrbuf[i] = i % ('z' - '0') + '0';
	}

	for (int i = 0; i < 32; i++) {
		INFO("start to pp_dv_post_send\n");
		ret = pp_dv_post_send(&ppdv->ppc, &ppdv->qp, &server, num_post,
				opcode, IBV_SEND_SIGNALED);
		if (ret) {
			ERR("pp_dv_post_send failed\n");
			return ret;
		}

		INFO("after pp_dv_post_send\n");
		num_comp = 0;
		while (num_comp < num_post) {
			ret = pp_dv_poll_cq(&ppdv->cq, 1);
			usleep(1000 * 10);
			if (ret == CQ_POLL_ERR) {
				ERR("poll_cq(send) failed %d, %d/%d\n", ret, num_comp, num_post);
				return ret;
			}
			if (ret > 0)
				num_comp++;
		}
		if (i < 32)
			usleep(1000 * 1000 * 1);
	}

	/* Reset the buffer so that we can check it the received data is expected */
	for (i = 0; i < num_post; i++)
		memset(ppdv->ppc.mrbuf[i], 0, ppdv->ppc.mrbuflen);

	INFO("Send done (num_post %d), now recving reply...\n", num_post);
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
		int ret = pp_dv_poll_cq(&(ppdv.mkey_modify_cq), 1);
		if (ret == CQ_POLL_ERR) {
			ERR("polling_mkey_modify_cq failed %d\n", ret);
			return NULL;
		}

		if (ret > 0) {
			INFO("polling_mkey_modify_cq got Mkey Modify CQE\n");
			INFO("start to rebuild mkey and alias mkey\n");
			
			//usleep(1000 * 1000 * 2);
			//ret = pp_ctx_init(&ppdv.ppc2, "mlx5_7", 0, NULL, true);
			//pp_allow_other_vhca_access(&ppdv.ppc2);
			pp_init_mkey(&ppdv.ppc2);
			pp_init_alias_mkey(&ppdv.ppc, &ppdv.ppc2, ppdv.mkey_modify_cq.cqn);
		}
		usleep(1000 * 10);
	}
	return NULL;
}

void sig_handler(int sig) {
    switch(sig)
    {
        case SIGUSR1:
	    pp_destroy_mkey(&(ppdv.ppc2));
            break;

        case SIGUSR2:
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

	ret = pp_ctx_init(&ppdv.ppc, ibv_devname, 0, NULL, true);
	if (ret)
		return ret;

	ret = pp_ctx_init(&ppdv.ppc2, "mlx5_7", 0, NULL, true);
	if (ret)
		return ret;

	// Set the mkey to be accesseable by alias mkey
	uint8_t access_key[32];
	for (int i = 0; i < 32; i++) {
		access_key[i] = 'a' + i;
	}
	for (int i = 0; i < PP_MAX_WR; i++) {
		uint32_t mkey = ppdv.ppc2.mkey[i];
		INFO("%d mkey %#x\n", i, mkey);
		ret = allow_other_vhca_access(ppdv.ppc2.ibctx,
					access_key,
					32,
					mkey);
		if (ret != true) {
			ERR("failed to set other vhca access %d\n", i);
			return 2;
		}
	}

	ret = pp_create_cq_dv(&ppdv.ppc, &ppdv.mkey_modify_cq);
	if (ret) {
		ERR("failed to pp_create_cq_dv mkey_modify_cq\n");
		return 3;
	}

    	pthread_t thread;
    	if (pthread_create(&thread, NULL, polling_mkey_modify_cq, 0) != 0) {
        	ERR("Failed to create thread");
        	return 1;
    	}
    	pthread_detach(thread);

	for (int i = 0; i < PP_MAX_WR; i++) {
		uint32_t mkey = ppdv.ppc2.mkey[i];
		uint32_t alias_mkey;
		struct mlx5dv_devx_obj *obj = create_alias_mkey_obj(ppdv.ppc.ibctx,
					ppdv.ppc2.vhca_id,
					mkey,
					access_key,
					32,
					ppdv.ppc.pdn,	
					ppdv.mkey_modify_cq.cqn,
					&alias_mkey);
		if (obj == NULL) {
			return 3;
		}
		ppdv.ppc.alias_mkey_obj[i] = obj;
		ppdv.ppc.alias_mkey[i] = alias_mkey;
		ppdv.ppc.alias_mkey_mrbuf[i] = ppdv.ppc2.mrbuf[i];

		// TEMP:Use the alias mkey to cover the mr. then we do not need to change those code.
		ppdv.ppc.mr[i]->lkey = alias_mkey;
		ppdv.ppc.mrbuf[i] = ppdv.ppc.alias_mkey_mrbuf[i];
	}

	ret = pp_create_cq_dv(&ppdv.ppc, &ppdv.cq);
	if (ret)
		goto out_create_cq;

	ret = pp_create_qp_dv(&ppdv.ppc, &ppdv.cq, &ppdv.qp);
	if (ret)
		goto out_create_qp;

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
out_create_cq:
	pp_ctx_cleanup(&ppdv.ppc);
	return ret;
}
