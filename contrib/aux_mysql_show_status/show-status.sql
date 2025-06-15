CREATE TABLE IF NOT EXISTS mysql_status_variables (
    variable_name VARCHAR(255) PRIMARY KEY,
    pg_source TEXT NOT NULL,
    description TEXT,
    category VARCHAR(100)
);

-- 清空并插入状态变量映射
TRUNCATE TABLE mysql_status_variables;

INSERT INTO mysql_status_variables (variable_name, pg_source, description, category) VALUES
-- 连接相关状态
('Threads_connected', 
 'SELECT count(*)::text FROM pg_stat_activity WHERE state = ''active''',
 '当前活跃连接数', 'connection'),

('Max_used_connections', 
 'SELECT setting FROM pg_settings WHERE name = ''max_connections''',
 '最大连接数限制', 'connection'),

('Connections', 
 'SELECT COALESCE(sum(numbackends), 0)::text FROM pg_stat_database',
 '总连接数', 'connection'),

-- 查询相关状态  
('Questions', 
 'SELECT COALESCE(sum(calls), 0)::text FROM pg_stat_statements',
 '执行的查询总数', 'query'),

('Queries', 
 'SELECT COALESCE(sum(calls), 0)::text FROM pg_stat_statements',
 '执行的查询总数', 'query'),

('Slow_queries', 
 'SELECT COALESCE(count(*), 0)::text FROM pg_stat_statements WHERE mean_exec_time > 1000',
 '慢查询数量(>1秒)', 'query'),

-- 表相关状态
('Open_tables', 
 'SELECT count(*)::text FROM pg_stat_user_tables',
 '打开的表数量', 'table'),

('Opened_tables', 
 'SELECT count(*)::text FROM pg_stat_user_tables',
 '已打开的表数量', 'table'),

-- 缓存和索引状态
('Key_reads', 
 'SELECT COALESCE(sum(idx_blks_read), 0)::text FROM pg_stat_user_indexes',
 '索引块从磁盘读取次数', 'cache'),

('Key_read_requests', 
 'SELECT COALESCE(sum(idx_blks_read + idx_blks_hit), 0)::text FROM pg_stat_user_indexes',
 '索引读取请求总数', 'cache'),

-- 数据读写状态
('Innodb_data_reads', 
 'SELECT COALESCE(sum(heap_blks_read), 0)::text FROM pg_stat_user_tables',
 '数据读取次数', 'innodb'),

('Innodb_data_writes', 
 'SELECT COALESCE(sum(heap_blks_hit), 0)::text FROM pg_stat_user_tables',
 '数据写入次数', 'innodb'),

-- 服务器运行状态
('Uptime', 
 'SELECT extract(epoch FROM (now() - pg_postmaster_start_time()))::bigint::text',
 '服务器运行时间(秒)', 'general'),

('Uptime_since_flush_status', 
 'SELECT COALESCE(extract(epoch FROM (now() - stats_reset))::bigint, 0)::text FROM pg_stat_bgwriter LIMIT 1',
 '自上次统计重置以来的时间', 'general'),

-- 处理器相关状态
('Handler_read_first', 
 'SELECT COALESCE(sum(idx_scan), 0)::text FROM pg_stat_user_indexes',
 '索引第一个条目读取次数', 'handler'),

('Handler_read_key', 
 'SELECT COALESCE(sum(idx_tup_read), 0)::text FROM pg_stat_user_indexes',
 '基于键读取行的请求次数', 'handler'),

('Handler_read_next', 
 'SELECT COALESCE(sum(idx_tup_fetch), 0)::text FROM pg_stat_user_indexes',
 '按键顺序读取下一行的请求次数', 'handler'),

-- 线程相关状态
('Threads_cached', 
 'SELECT ''0''',
 '线程缓存中的线程数', 'thread'),

('Threads_created', 
 'SELECT COALESCE(sum(numbackends), 0)::text FROM pg_stat_database',
 '创建的线程数', 'thread'),

('Threads_running', 
 'SELECT count(*)::text FROM pg_stat_activity WHERE state = ''active''',
 '非休眠状态的线程数', 'thread');

-- 创建获取单个状态变量值的函数
CREATE OR REPLACE FUNCTION mysql_get_status_variable(var_name VARCHAR)
RETURNS VARCHAR AS $$
DECLARE
    query_text TEXT;
    result VARCHAR;
BEGIN
    -- 获取对应的 PostgreSQL 查询
    SELECT pg_source INTO query_text 
    FROM mysql_status_variables 
    WHERE variable_name = var_name;
    
    IF query_text IS NULL THEN
        RETURN '0';
    END IF;
    
    -- 执行查询并返回结果
    BEGIN
        EXECUTE query_text INTO result;
    EXCEPTION
        WHEN OTHERS THEN
            result := '0';
    END;
    
    RETURN COALESCE(result, '0');
END;
$$ LANGUAGE plpgsql STABLE;

-- 创建 SHOW STATUS 主函数
CREATE OR REPLACE FUNCTION mysql_show_status()
RETURNS TABLE(variable_name VARCHAR, value VARCHAR) AS $$
BEGIN
    RETURN QUERY
    SELECT 
        msv.variable_name,
        mysql_get_status_variable(msv.variable_name) as value
    FROM mysql_status_variables msv
    ORDER BY msv.variable_name;
END;
$$ LANGUAGE plpgsql STABLE;

-- 创建 SHOW STATUS LIKE 函数
CREATE OR REPLACE FUNCTION mysql_show_status_like(pattern VARCHAR)
RETURNS TABLE(variable_name VARCHAR, value VARCHAR) AS $$
BEGIN
    RETURN QUERY
    SELECT 
        msv.variable_name,
        mysql_get_status_variable(msv.variable_name) as value
    FROM mysql_status_variables msv
    WHERE msv.variable_name ILIKE pattern
    ORDER BY msv.variable_name;
END;
$$ LANGUAGE plpgsql STABLE;

-- 创建 SHOW GLOBAL STATUS 函数（与 SHOW STATUS 相同）
CREATE OR REPLACE FUNCTION mysql_show_global_status()
RETURNS TABLE(variable_name VARCHAR, value VARCHAR) AS $$
BEGIN
    RETURN QUERY SELECT * FROM mysql_show_status();
END;
$$ LANGUAGE plpgsql STABLE;

-- 创建获取单个状态变量的便捷函数
CREATE OR REPLACE FUNCTION mysql_get_status(var_name VARCHAR)
RETURNS VARCHAR AS $$
BEGIN
    RETURN mysql_get_status_variable(var_name);
END;
$$ LANGUAGE plpgsql STABLE;

-- 创建视图以便更容易访问
CREATE OR REPLACE VIEW mysql_status AS
SELECT * FROM mysql_show_status();

CREATE OR REPLACE VIEW mysql_global_status AS  
SELECT * FROM mysql_show_global_status();

-- 创建兼容的存储过程（模拟 MySQL 命令）
CREATE OR REPLACE FUNCTION show_status()
RETURNS TABLE(variable_name VARCHAR, value VARCHAR) AS $$
BEGIN
    RETURN QUERY SELECT * FROM mysql_show_status();
END;
$$ LANGUAGE plpgsql STABLE;

CREATE OR REPLACE FUNCTION show_status_like(pattern VARCHAR)
RETURNS TABLE(variable_name VARCHAR, value VARCHAR) AS $$
BEGIN
    RETURN QUERY SELECT * FROM mysql_show_status_like(pattern);
END;
$$ LANGUAGE plpgsql STABLE;

CREATE OR REPLACE FUNCTION show_global_status()
RETURNS TABLE(variable_name VARCHAR, value VARCHAR) AS $$
BEGIN
    RETURN QUERY SELECT * FROM mysql_show_global_status();
END;
$$ LANGUAGE plpgsql STABLE;

-- 授予必要的权限
GRANT SELECT ON mysql_status_variables TO PUBLIC;
GRANT SELECT ON mysql_status TO PUBLIC;
GRANT SELECT ON mysql_global_status TO PUBLIC;
GRANT EXECUTE ON FUNCTION mysql_show_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION mysql_show_status_like(VARCHAR) TO PUBLIC;
GRANT EXECUTE ON FUNCTION mysql_show_global_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION mysql_get_status(VARCHAR) TO PUBLIC;
GRANT EXECUTE ON FUNCTION show_status() TO PUBLIC;
GRANT EXECUTE ON FUNCTION show_status_like(VARCHAR) TO PUBLIC;
GRANT EXECUTE ON FUNCTION show_global_status() TO PUBLIC;

-- 测试功能
DO $$
BEGIN
    RAISE NOTICE '=== MySQL SHOW STATUS Extension 安装完成 ===';
    RAISE NOTICE '使用示例:';
    RAISE NOTICE '  SELECT * FROM mysql_show_status();';
    RAISE NOTICE '  SELECT * FROM mysql_show_status_like(''Thread%%'');';
    RAISE NOTICE '  SELECT mysql_get_status(''Uptime'');';
    RAISE NOTICE '===============================================';
END $$;

-- 运行一个简单测试
SELECT 'MySQL SHOW STATUS Extension 已成功安装!' as status;
SELECT count(*) as total_variables FROM mysql_status_variables;
SELECT 'Uptime: ' || mysql_get_status('Uptime') || ' seconds' as server_uptime;
